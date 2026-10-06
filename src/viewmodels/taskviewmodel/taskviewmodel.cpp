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
using std::function;
using std::exception_ptr;
using std::shared_ptr;
using std::invalid_argument;

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
    case business::ErrorCode::internal:
        return TaskViewModel::tr("Operation failed: %1").arg(QString::fromUtf8(error.detail));
    }
    return {};
}
} // namespace

struct TaskViewModel::Impl
{
    using Completion = function<void(exception_ptr, business::Update)>;
    enum class Operation { reload, add, update, remove };
    shared_ptr<business::TaskService> service;
    explicit Impl(shared_ptr<business::TaskService> source) : service(std::move(source))
    {
        if (!service)
            throw invalid_argument("A task service is required.");
    }

    static auto completion(TaskViewModel *viewModel, Operation operation) -> Completion
    {
        // Only access the QPointer on the GUI thread. The app outlives all services.
        const auto guard = QPointer<TaskViewModel>{viewModel};
        auto *application = QCoreApplication::instance();
        return [guard, application, operation](exception_ptr failure, business::Update update) mutable {
            QMetaObject::invokeMethod(application, [guard, operation, failure, update = std::move(update)] {
                if (!guard)
                    return;
                Q_ASSERT(QThread::currentThread() == guard->thread());
                if (guard->m_stopping) {
                    guard->setBusy(false);
                    return;
                }
                if (failure) {
                    try { std::rethrow_exception(failure); }
                    catch (const exception &error) { guard->setError(QString::fromUtf8(error.what())); }
                    catch (...) { guard->setError(TaskViewModel::tr("Operation failed.")); }
                    guard->setBusy(false);
                    return;
                }
                if (update.changed) {
                    auto items = QList<TaskItem>{};
                    items.reserve(static_cast<qsizetype>(update.tasks.size()));
                    for (const auto &task : update.tasks)
                        items.append({QString::fromUtf8(task.id), QString::fromUtf8(task.title), task.completed});
                    guard->m_tasks.applyTasks(std::move(items));
                    emit guard->countsChanged();
                }
                guard->setReady(update.ready);
                guard->setError(update.error ? translatedError(*update.error) : QString{});
                guard->setBusy(false);
                if (operation == Operation::add && update.changed && !update.error)
                    emit guard->taskAdded();
            }, Qt::QueuedConnection);
        };
    }
    void run(TaskViewModel *viewModel, Operation operation, asio::awaitable<business::Update> task)
    {
        asio::co_spawn(service->executor(), std::move(task), completion(viewModel, operation));
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
    if (m_stopping || m_busy || m_ready)
        throw std::logic_error("Initialize an idle ViewModel before using it.");
    setBusy(true);
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
bool TaskViewModel::busy() const { return m_busy; }
QString TaskViewModel::errorMessage() const { return m_errorMessage; }

bool TaskViewModel::addTask(const QString &title)
{
    if (m_stopping || !m_ready || m_busy)
        return false;
    setBusy(true);
    m_impl->run(this, Impl::Operation::add, m_impl->service->addTask(utf8(title)));
    return true;
}
bool TaskViewModel::setTaskCompleted(const QString &id, bool completed)
{
    if (m_stopping || !m_ready || m_busy)
        return false;
    setBusy(true);
    m_impl->run(this, Impl::Operation::update, m_impl->service->setTaskCompleted(utf8(id), completed));
    return true;
}
bool TaskViewModel::removeTask(const QString &id)
{
    if (m_stopping || !m_ready || m_busy)
        return false;
    setBusy(true);
    m_impl->run(this, Impl::Operation::remove, m_impl->service->removeTask(utf8(id)));
    return true;
}
bool TaskViewModel::reload()
{
    if (m_stopping || m_busy)
        return false;
    setBusy(true);
    m_impl->run(this, Impl::Operation::reload, m_impl->service->reload());
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
void TaskViewModel::setBusy(bool busy)
{
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged();
}
