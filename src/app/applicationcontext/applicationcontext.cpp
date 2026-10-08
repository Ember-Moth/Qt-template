#include "app/applicationcontext/applicationcontext.h"
#include "viewmodels/async/spawn.h"
#include <QCoreApplication>
#include <QDebug>
#include <QThread>

import std;
import Template.App.Context;
import Template.Runtime.Asio;
import Template.Tasks;
import Template.Storage.Mmkv;
import Template.Network.Http;
import Template.App.AsyncMain;

using std::string;
using std::shared_ptr;
using std::function;
using std::vector;
using std::exception;
using std::make_shared;
using std::make_unique;
using std::rethrow_exception;
namespace fs = std::filesystem;
using runtime::AsioRuntime;
using runtime::Task;
using storage::MmkvStore;
using network::HttpClient;
using business::TaskService;
using application::Lifecycle;
using application::Dependencies;
using application::async_main;
using application::startup_step;
using std::exception_ptr;
using viewmodels::spawn;

namespace {
auto nativePath(const QString &path)
{
#ifdef _WIN32
    return fs::path(path.toStdWString());
#else
    const auto utf8 = path.toUtf8();
    return fs::path(string(utf8.constData(), utf8.size()));
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
    AsioRuntime asyncRuntime;
    shared_ptr<MmkvStore> store;
    // Shared HTTP backend for services that call remote APIs, such as business::TaskImportService.
    shared_ptr<HttpClient> http;
    shared_ptr<Lifecycle> lifecycle;
    Dependencies dependencies;
    vector<Attachment> attachments;
    // Each feature: its service, then the ViewModel presenting it, then one attach() call.
    shared_ptr<TaskService> taskService;
    TaskViewModel taskViewModel;
    bool started = false;
    bool stopping = false;

    Impl(const QString &directory, QObject *owner)
        : store(make_shared<MmkvStore>(nativePath(directory))),
          http(make_shared<HttpClient>(asyncRuntime.context())),
          lifecycle(make_shared<Lifecycle>(asyncRuntime.executor())),
          dependencies{lifecycle, {}},
          taskService(make_shared<TaskService>(asyncRuntime.executor(), store)),
          taskViewModel(TaskViewModel::Dependencies{taskService, asyncRuntime.executor()}, owner)
    {
        attach(taskViewModel, taskService, &TaskService::reload);
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
    void attach(ViewModel &viewModel, shared_ptr<Service> service, Task<Result> (Service::*operation)())
    {
        const auto startup = lifecycle->startup<Result>();
        dependencies.startup.push_back(startup_step(std::move(service), operation, startup));
        attachments.push_back({
            [&viewModel, startup] { viewModel.initialize({startup.result()}); },
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
            spawn(asyncRuntime.executor(), async_main(dependencies),
                QCoreApplication::instance(), [](exception_ptr failure) {
                    if (!failure) return;
                    try { rethrow_exception(failure); }
                    catch (const exception &error) { qCritical() << "async_main:" << error.what(); }
                    catch (...) { qCritical() << "async_main failed."; }
                    QCoreApplication::exit(1);
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
        // Requests in flight end now instead of at their timeouts while the runtime drains.
        http->stop();
        lifecycle->requestStop();
    }
};

ApplicationContext::ApplicationContext(const QString &storageDirectory, QObject *parent)
    : QObject(parent), m_impl(make_unique<Impl>(storageDirectory, this))
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
