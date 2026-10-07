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
using std::function;
using std::vector;

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
    struct Attachment
    {
        function<void()> initialize;
        function<void()> stop;
    };

    // Stop the root coroutine before draining the runtime; Qt never has to reply to shutdown.
    runtime::AsioRuntime asyncRuntime;
    shared_ptr<storage::MmkvStore> store;
    shared_ptr<application::Lifecycle> lifecycle;
    application::Dependencies dependencies;
    vector<Attachment> attachments;
    // Each feature: its service, then the ViewModel presenting it, then one attach() call.
    shared_ptr<business::TaskService> taskService;
    TaskViewModel taskViewModel;
    bool started = false;
    bool stopping = false;

    Impl(const QString &directory, QObject *owner)
        : store(std::make_shared<storage::MmkvStore>(nativePath(directory))),
          lifecycle(std::make_shared<application::Lifecycle>(asyncRuntime.executor())),
          dependencies{lifecycle, {}},
          taskService(std::make_shared<business::TaskService>(asyncRuntime.executor(), store)),
          taskViewModel(TaskViewModel::Dependencies{taskService}, owner)
    {
        attach(taskViewModel, taskService, &business::TaskService::reload);
    }
    ~Impl()
    {
        stop();
        asyncRuntime.finish();
    }
    // A ViewModel without startup loading still stops with the application before the runtime drains.
    template <class ViewModel>
    void attach(ViewModel &viewModel)
    {
        attachments.push_back({{}, [&viewModel] { viewModel.stop(); }});
    }
    // async_main runs the service's startup operation, start() hands its result to the ViewModel,
    // and stop() reaches the ViewModel before the runtime drains.
    template <class ViewModel, class Service, class Result>
    void attach(ViewModel &viewModel, shared_ptr<Service> service, asio::awaitable<Result> (Service::*operation)())
    {
        const auto startup = lifecycle->startup<Result>();
        dependencies.startup.push_back(application::startup_step(std::move(service), operation, startup));
        attachments.push_back({
            [this, &viewModel, startup] { viewModel.initialize({asyncRuntime.executor(), startup.result()}); },
            [&viewModel] { viewModel.stop(); },
        });
    }
    bool start()
    {
        if (started || stopping) return false;
        started = true;
        try {
            for (const auto &attachment : attachments)
                if (attachment.initialize) attachment.initialize();
            auto *app = QCoreApplication::instance();
            asio::co_spawn(asio::make_strand(asyncRuntime.executor()),
                application::async_main(dependencies), [app](std::exception_ptr failure) {
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
        for (const auto &attachment : attachments)
            attachment.stop();
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
