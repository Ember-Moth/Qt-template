#include "viewmodels/taskviewmodel/taskviewmodel.h"
#include "viewmodels/async/spawn.h"
#include <QCoreApplication>
#include <QThread>

import std;
import Template.Tasks;
import Template.ViewModels.Task;

using std::string;
using std::size_t;
using std::exception;
using std::optional;
using std::shared_ptr;
using std::map;
using std::pair;
using std::make_unique;
using std::erase_if;
using std::exception_ptr;
using std::rethrow_exception;
using std::ranges::none_of;
using runtime::Task;
using runtime::Executor;
using business::Error;
using business::ErrorCode;
using business::Exception;
using business::TaskRecord;
using business::TaskService;
using business::UpdateResult;
using viewmodels::spawn;

namespace {
string utf8(const QString &text)
{
    const auto encoded = text.toUtf8();
    return {encoded.constData(), static_cast<size_t>(encoded.size())};
}
QString translatedError(const Error &error)
{
    switch (error.code) {
    case ErrorCode::invalidTitle:
        return TaskViewModel::tr("Enter a task between 1 and %1 characters.").arg(static_cast<int>(TaskRecord::maxTitleLength));
    case ErrorCode::notFound: return TaskViewModel::tr("This task no longer exists.");
    case ErrorCode::notReady: return TaskViewModel::tr("Load the tasks before making changes.");
    case ErrorCode::invalidRecord: return TaskViewModel::tr("Invalid or duplicate task record.");
    case ErrorCode::invalidFormat: return TaskViewModel::tr("Unsupported or invalid task file format.");
    case ErrorCode::storage:
        return TaskViewModel::tr("Storage error: %1").arg(QString::fromUtf8(error.detail));
    case ErrorCode::network:
        return TaskViewModel::tr("Network error: %1").arg(QString::fromUtf8(error.detail));
    case ErrorCode::cancelled: return TaskViewModel::tr("The operation was cancelled.");
    case ErrorCode::missingDependency:
    case ErrorCode::contractViolation:
        return TaskViewModel::tr("Operation failed: %1").arg(QString::fromUtf8(error.detail));
    }
    return {};
}
} // namespace

struct TaskViewModel::Impl
{
    enum class Operation { reload, add, update, remove };
    // Updates and removals share the same task target; loading and adding have their own targets.
    using ErrorTarget = pair<Operation, QString>;
    map<ErrorTarget, QString> errors;
    shared_ptr<TaskService> service;
    Executor executor;
    explicit Impl(Dependencies dependencies) : service(std::move(dependencies.service)), executor(std::move(dependencies.executor))
    {
        if (!service)
            throw Exception({ErrorCode::missingDependency, "TaskViewModel: a task service is required"});
        if (!executor)
            throw Exception({ErrorCode::missingDependency, "TaskViewModel: an Asio executor is required"});
    }

    void setOperationError(TaskViewModel &viewModel, Operation operation, const QString &id, QString error)
    {
        const auto target = ErrorTarget{operation == Operation::remove ? Operation::update : operation, id};
        if (error.isEmpty())
            errors.erase(target);
        else
            errors[target] = std::move(error);

        auto message = QString{};
        for (const auto &[key, detail] : errors) {
            if (!message.isEmpty()) message += QLatin1Char('\n');
            message += detail;
        }
        viewModel.setError(std::move(message));
    }

    // A task that left the list cannot be retried, so its error leaves with it.
    void forgetMissingTasks(const QList<TaskItem> &tasks)
    {
        erase_if(errors, [&tasks](const auto &entry) {
            const auto &[operation, id] = entry.first;
            return operation == Operation::update
                && none_of(tasks, [&id](const TaskItem &task) { return task.id == id; });
        });
    }

    // Runs on the GUI thread once a command or the startup result finishes. The startup result is empty
    // when the application stopped first.
    void apply(TaskViewModel &viewModel, Operation operation, const QString &id, exception_ptr failure,
               optional<UpdateResult> outcome)
    {
        Q_ASSERT(QThread::currentThread() == viewModel.thread());
        auto added = false;
        if (viewModel.m_stopping) {
            // Shutting down: release the operation without touching the view.
        } else if (failure) {
            // Only unrecoverable failures throw; the committed state is unchanged.
            try { rethrow_exception(failure); }
            catch (const exception &error) {
                setOperationError(viewModel, operation, id,
                    TaskViewModel::tr("Operation failed: %1").arg(QString::fromUtf8(error.what())));
            }
            catch (...) {
                setOperationError(viewModel, operation, id, TaskViewModel::tr("Operation failed."));
            }
        } else if (outcome && *outcome) {
            // Every update is a full snapshot from the service strand, applied in completion order.
            const auto &update = **outcome;
            if (update.changed) {
                auto items = QList<TaskItem>{};
                items.reserve(static_cast<qsizetype>(update.tasks.size()));
                for (const auto &task : update.tasks)
                    items.append({QString::fromUtf8(task.id), QString::fromUtf8(task.title), task.completed});
                viewModel.m_tasks.applyTasks(std::move(items));
                forgetMissingTasks(viewModel.m_tasks.tasks());
                emit viewModel.countsChanged();
            }
            viewModel.setReady(update.ready);
            setOperationError(viewModel, operation, id, {});
            added = operation == Operation::add && update.changed;
        } else if (outcome) {
            // A failed load leaves nothing usable; a failed command keeps the committed tasks.
            if (operation == Operation::reload)
                viewModel.setReady(false);
            setOperationError(viewModel, operation, id, translatedError(outcome->error()));
        }
        // An empty result is a cancelled startup; async_main reports the cause at the application boundary.
        finish(viewModel, operation, id);
        // The input unlocks before it is cleared, so the view can focus it again.
        if (added)
            emit viewModel.taskAdded();
    }
    static void finish(TaskViewModel &viewModel, Operation operation, const QString &id)
    {
        switch (operation) {
        case Operation::reload: viewModel.setLoading(false); break;
        case Operation::add: viewModel.setAdding(false); break;
        case Operation::update:
        case Operation::remove: viewModel.m_tasks.setPending(id, false); break;
        }
        viewModel.changePending(-1);
    }
    // Commands produce an UpdateResult; the startup result may also be empty.
    template <class Result>
    void run(TaskViewModel *viewModel, Operation operation, QString id, Task<Result> task)
    {
        viewModel->changePending(1);
        spawn(executor, std::move(task), viewModel, [viewModel, operation, id](exception_ptr failure, Result result) {
            viewModel->m_impl->apply(*viewModel, operation, id, failure, optional<UpdateResult>(std::move(result)));
        });
    }
};

TaskViewModel::TaskViewModel(Dependencies dependencies, QObject *parent)
    : QObject(parent), m_impl(make_unique<Impl>(std::move(dependencies))), m_tasks(this)
{
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
}
TaskViewModel::~TaskViewModel() = default;
void TaskViewModel::initialize(Initialization initialization)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping || m_pending > 0 || m_ready)
        throw Exception({ErrorCode::contractViolation,
            QStringLiteral("TaskViewModel::initialize: call it once on an idle ViewModel (stopping=%1, pending=%2, ready=%3)")
                .arg(m_stopping ? QStringLiteral("true") : QStringLiteral("false"))
                .arg(m_pending)
                .arg(m_ready ? QStringLiteral("true") : QStringLiteral("false")).toStdString()});
    setLoading(true);
    m_impl->run(this, Impl::Operation::reload, {}, std::move(initialization.result));
}
void TaskViewModel::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_stopping = true;
    setReady(false);
}
TaskListModel *TaskViewModel::tasks() { return &m_tasks; }
int TaskViewModel::totalCount() const { return m_tasks.rowCount(); }
int TaskViewModel::remainingCount() const { return totalCount() - m_tasks.completedCount(); }
bool TaskViewModel::ready() const { return m_ready; }
bool TaskViewModel::busy() const { return m_pending > 0; }
bool TaskViewModel::loading() const { return m_loading; }
bool TaskViewModel::adding() const { return m_adding; }
QString TaskViewModel::errorMessage() const { return m_errorMessage; }

bool TaskViewModel::accept(bool targetPending)
{
    auto reason = QString{};
    if (m_stopping)
        reason = tr("The application is closing.");
    else if (m_loading || !m_ready)
        reason = tr("Load the tasks before making changes.");
    else if (targetPending)
        reason = tr("Wait for the current change to finish.");
    if (reason.isEmpty())
        return true;
    emit commandRejected(reason);
    return false;
}
bool TaskViewModel::addTask(const QString &title)
{
    // One add at a time: the input is cleared only after its save succeeds.
    if (!accept(m_adding))
        return false;
    setAdding(true);
    m_impl->run(this, Impl::Operation::add, {}, m_impl->service->addTask(utf8(title)));
    return true;
}
bool TaskViewModel::setTaskCompleted(const QString &id, bool completed)
{
    if (!accept(m_tasks.isPending(id)))
        return false;
    m_tasks.setPending(id, true);
    m_impl->run(this, Impl::Operation::update, id, m_impl->service->setTaskCompleted(utf8(id), completed));
    return true;
}
bool TaskViewModel::removeTask(const QString &id)
{
    if (!accept(m_tasks.isPending(id)))
        return false;
    m_tasks.setPending(id, true);
    m_impl->run(this, Impl::Operation::remove, id, m_impl->service->removeTask(utf8(id)));
    return true;
}
bool TaskViewModel::reload()
{
    // A reload replaces every row, so it waits for accepted changes and blocks new ones.
    auto reason = QString{};
    if (m_stopping)
        reason = tr("The application is closing.");
    else if (m_pending > 0)
        reason = tr("Wait for the current change to finish.");
    if (!reason.isEmpty()) {
        emit commandRejected(reason);
        return false;
    }
    setLoading(true);
    m_impl->run(this, Impl::Operation::reload, {}, m_impl->service->reload());
    return true;
}
void TaskViewModel::setError(QString error)
{
    if (m_errorMessage == error) return;
    m_errorMessage = std::move(error);
    emit errorMessageChanged();
}
void TaskViewModel::setReady(bool ready)
{
    if (m_ready == ready) return;
    m_ready = ready;
    emit readyChanged();
}
void TaskViewModel::setLoading(bool loading)
{
    if (m_loading == loading) return;
    m_loading = loading;
    emit loadingChanged();
}
void TaskViewModel::setAdding(bool adding)
{
    if (m_adding == adding) return;
    m_adding = adding;
    emit addingChanged();
}
void TaskViewModel::changePending(int delta)
{
    const auto wasBusy = busy();
    m_pending += delta;
    Q_ASSERT(m_pending >= 0);
    if (busy() != wasBusy)
        emit busyChanged();
}
