#include "app/applicationcontext/applicationcontext.h"
#include <asio/co_spawn.hpp>
#include <asio/strand.hpp>
#include <QCoreApplication>
#include <QDebug>
#include <QThread>

import std;
import Template.App.Context;
import Template.Runtime.Asio;
import Template.Tasks;
import Template.Storage.Mmkv;
import Template.App.AsyncMain;

using std::string;
using std::shared_ptr;

namespace {
auto nativePath(const QString &path)
{
#ifdef _WIN32
    return std::filesystem::path(path.toStdWString());
#else
    const auto utf8 = path.toUtf8();
    return std::filesystem::path(string(utf8.constData(), utf8.size()));
#endif
}
} // namespace

struct ApplicationContext::Impl
{
    // Stop the root coroutine before draining the runtime; Qt never has to reply to shutdown.
    runtime::AsioRuntime asyncRuntime;
    shared_ptr<storage::MmkvStore> store;
    shared_ptr<business::TaskService> taskService;
    shared_ptr<application::Lifecycle> lifecycle;
    TaskViewModel taskViewModel;
    bool started = false;
    bool stopping = false;

    Impl(const QString &directory, QObject *owner)
        : store(std::make_shared<storage::MmkvStore>(nativePath(directory))),
          taskService(std::make_shared<business::TaskService>(asyncRuntime.executor(), store)),
          lifecycle(std::make_shared<application::Lifecycle>(asyncRuntime.executor())),
          taskViewModel(TaskViewModel::Dependencies{taskService}, owner) {}
    ~Impl()
    {
        stop();
        asyncRuntime.finish();
    }
    bool start()
    {
        if (started || stopping) return false;
        started = true;
        try {
            taskViewModel.initialize({asyncRuntime.executor(), lifecycle->startup()});
            auto *app = QCoreApplication::instance();
            asio::co_spawn(asio::make_strand(asyncRuntime.executor()),
                application::async_main({taskService, lifecycle}), [app](std::exception_ptr failure) {
                    if (!failure) return;
                    QMetaObject::invokeMethod(app, [failure] {
                        try { std::rethrow_exception(failure); }
                        catch (const std::exception &error) { qCritical() << "async_main:" << error.what(); }
                        catch (...) { qCritical() << "async_main failed."; }
                        QCoreApplication::exit(1);
                    }, Qt::QueuedConnection);
                });
        } catch (...) {
            stop();
            throw;
        }
        return true;
    }
    void stop()
    {
        if (stopping) return;
        stopping = true;
        taskViewModel.stop();
        lifecycle->requestStop();
    }
};

ApplicationContext::ApplicationContext(const QString &storageDirectory, QObject *parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(storageDirectory, this))
{
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &ApplicationContext::stop);
}
ApplicationContext::~ApplicationContext() = default;
TaskViewModel *ApplicationContext::tasks() { return &m_impl->taskViewModel; }
bool ApplicationContext::start()
{
    Q_ASSERT(QThread::currentThread() == thread());
    return m_impl->start();
}
void ApplicationContext::stop()
{
    Q_ASSERT(QThread::currentThread() == thread());
    m_impl->stop();
}
