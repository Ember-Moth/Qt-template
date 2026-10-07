#include <asio/co_spawn.hpp>
#include "viewmodels/taskviewmodel/taskviewmodel.h"
#include <QCoreApplication>
#include <QPointer>
#include <QThread>

import std;
import Template.Tasks;
import Template.ViewModels.Task;

using std::string;
using std::size_t;
using std::exception;
using std::optional;
using std::exception_ptr;
using std::shared_ptr;

namespace {
string utf8(const QString &text)
{
    const auto encoded = text.toUtf8();
    return {encoded.constData(), static_cast<size_t>(encoded.size())};
}
QString translatedError(const business::Error &error)
{
    switch (error.code) {
    case business::ErrorCode::invalidTitle:
        return TaskViewModel::tr("Enter a task between 1 and %1 characters.").arg(static_cast<int>(business::Task::maxTitleLength));
    case business::ErrorCode::notFound: return TaskViewModel::tr("This task no longer exists.");
    case business::ErrorCode::notReady: return TaskViewModel::tr("Load the tasks before making changes.");
    case business::ErrorCode::invalidRecord: return TaskViewModel::tr("Invalid or duplicate task record.");
    case business::ErrorCode::invalidFormat: return TaskViewModel::tr("Unsupported or invalid task file format.");
    case business::ErrorCode::storage:
        return TaskViewModel::tr("Storage error: %1").arg(QString::fromUtf8(error.detail));
    case business::ErrorCode::missingDependency:
    case business::ErrorCode::contractViolation:
        return TaskViewModel::tr("Operation failed: %1").arg(QString::fromUtf8(error.detail));
    }
    return {};
}
} // namespace

struct TaskViewModel::Impl
{
    enum class Operation { reload, add, update, remove };
    shared_ptr<business::TaskService> service;
    explicit Impl(shared_ptr<business::TaskService> source) : service(std::move(source))
    {
        if (!service)
            throw business::Exception({business::ErrorCode::missingDependency, "TaskViewModel: a task service is required"});
    }

    // Commands complete with an UpdateResult; initialization may also complete empty when cancelled.
    static auto completion(TaskViewModel *viewModel, Operation operation, QString id = {})
    {
        // Only access the QPointer on the GUI thread. The app outlives all services.
        const auto guard = QPointer<TaskViewModel>{viewModel};
        auto *application = QCoreApplication::instance();
        return [guard, application, operation, id](exception_ptr failure, auto result) mutable {
            auto outcome = optional<business::UpdateResult>{std::move(result)};
            QMetaObject::invokeMethod(application, [guard, operation, id, failure, outcome = std::move(outcome)] {
                if (!guard)
                    return;
                Q_ASSERT(QThread::currentThread() == guard->thread());
                auto added = false;
                if (guard->m_stopping) {
                    // Shutting down: release the operation without touching the view.
                } else if (failure) {
                    // Only unrecoverable failures throw; the committed state is unchanged.
                    try { std::rethrow_exception(failure); }
                    catch (const exception &error) {
                        guard->setError(TaskViewModel::tr("Operation failed: %1").arg(QString::fromUtf8(error.what())));
                    }
                    catch (...) { guard->setError(TaskViewModel::tr("Operation failed.")); }
                } else if (outcome && *outcome) {
                    // Every update is a full snapshot from the service strand, applied in completion order.
                    const auto &update = **outcome;
                    if (update.changed) {
                        auto items = QList<TaskItem>{};
                        items.reserve(static_cast<qsizetype>(update.tasks.size()));
                        for (const auto &task : update.tasks)
                            items.append({QString::fromUtf8(task.id), QString::fromUtf8(task.title), task.completed});
                        guard->m_tasks.applyTasks(std::move(items));
                        emit guard->countsChanged();
                    }
                    guard->setReady(update.ready);
                    guard->setError({});
                    added = operation == Operation::add && update.changed;
                } else if (outcome) {
                    // A failed load leaves nothing usable; a failed command keeps the committed tasks.
                    if (operation == Operation::reload)
                        guard->setReady(false);
                    guard->setError(translatedError(outcome->error()));
                }
                // An empty outcome is a cancelled startup; async_main reports the cause at the application boundary.
                finish(*guard, operation, id);
                // The input unlocks before it is cleared, so the view can focus it again.
                if (added)
                    emit guard->taskAdded();
            }, Qt::QueuedConnection);
        };
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
    void run(TaskViewModel *viewModel, Operation operation, QString id, asio::awaitable<business::UpdateResult> task)
    {
        viewModel->changePending(1);
        asio::co_spawn(service->executor(), std::move(task), completion(viewModel, operation, std::move(id)));
    }
};

TaskViewModel::TaskViewModel(Dependencies dependencies, QObject *parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(std::move(dependencies.service))), m_tasks(this)
{
    Q_ASSERT(QCoreApplication::instance());
    Q_ASSERT(QThread::currentThread() == QCoreApplication::instance()->thread());
}
TaskViewModel::~TaskViewModel() = default;
void TaskViewModel::initialize(Initialization initialization)
{
    Q_ASSERT(QThread::currentThread() == thread());
    if (m_stopping || m_pending > 0 || m_ready)
        throw business::Exception({business::ErrorCode::contractViolation, std::format(
            "TaskViewModel::initialize: call it once on an idle ViewModel (stopping={}, pending={}, ready={})",
            m_stopping, m_pending, m_ready)});
    setLoading(true);
    changePending(1);
    asio::co_spawn(initialization.executor, std::move(initialization.result),
        Impl::completion(this, Impl::Operation::reload));
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
