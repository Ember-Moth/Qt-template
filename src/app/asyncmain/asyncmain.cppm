module;
#include "runtime/task.h"
// Startup is a template over Asio's channel, so this interface keeps the channel headers.
#include <asio/as_tuple.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/use_awaitable.hpp>

export module Template.App.AsyncMain;
import std;
export import Template.Errors;

using std::shared_ptr;
using std::function;
using std::vector;
using std::optional;
using std::make_shared;
using std::nullopt;
using asio::as_tuple;
using asio::error_code;
using asio::use_awaitable;
using asio::experimental::concurrent_channel;
using asio::experimental::error::channel_errors;
using runtime::Task;
using runtime::Executor;

namespace application::detail {
// Importers instantiate Asio channel templates that turn channel_errors into error_code through
// std::is_error_code_enum and ADL make_error_code. Reduced BMIs keep only global-fragment
// declarations the module references, so reference both here.
inline auto channelError(channel_errors code) -> error_code
{
    return code;
}
} // namespace application::detail

export namespace application {
// Application wiring failures; all are thrown as Exception because the run cannot continue.
enum class ErrorCode { missingDependency, contractViolation };
using Error = errors::Error<ErrorCode>;
using Exception = errors::Exception<ErrorCode>;

struct Dependencies;
auto async_main(Dependencies dependencies) -> Task<>;

// A one-shot startup result for a single consumer; stopping or a startup failure cancels it.
// Cancellation is an expected outcome of shutting down, so it is an empty result, not an error.
template <class Result>
class Startup
{
public:
    explicit Startup(Executor executor) : m_channel(make_shared<Channel>(std::move(executor), 1)) {}

    // Completes with the delivered result, or with nullopt once cancelled.
    auto result() const -> Task<optional<Result>> { return receive(m_channel); }
    auto deliver(Result result) const -> bool { return m_channel->try_send(error_code{}, std::move(result)); }
    void cancel() const { m_channel->close(); }

private:
    using Channel = concurrent_channel<void(error_code, Result)>;
    static auto receive(shared_ptr<Channel> channel) -> Task<optional<Result>>
    {
        auto [error, result] = co_await channel->async_receive(as_tuple(use_awaitable));
        if (error) co_return nullopt;
        co_return optional<Result>{std::move(result)};
    }
    shared_ptr<Channel> m_channel;
};

// One application run: the startup results registered for it and a latched shutdown notification.
class Lifecycle
{
public:
    explicit Lifecycle(Executor executor);
    ~Lifecycle();
    Lifecycle(const Lifecycle &) = delete;
    Lifecycle &operator=(const Lifecycle &) = delete;

    // Register every result before start(); one registered after stopping is cancelled at once.
    template <class Result>
    auto startup() -> Startup<Result>
    {
        auto result = Startup<Result>(executor());
        track([result] { result.cancel(); });
        return result;
    }
    void requestStop();

private:
    struct Impl;
    shared_ptr<Impl> m_impl;
    auto executor() const -> Executor;
    void track(function<void()> cancel);
    friend auto async_main(Dependencies dependencies) -> Task<>;
};

// Runs one service operation at startup and reports whether its consumer received the result.
using StartupStep = function<runtime::Task<bool>()>;

template <class Service, class Result>
auto run_startup(shared_ptr<Service> service, Task<Result> (Service::*operation)(), Startup<Result> startup)
    -> Task<bool>
{
    if (!service)
        throw Exception({ErrorCode::missingDependency, "async_main: a startup step has no service"});
    // The service runs the operation on its own strand.
    auto result = co_await ((*service).*operation)();
    co_return startup.deliver(std::move(result));
}

template <class Service, class Result>
auto startup_step(shared_ptr<Service> service, Task<Result> (Service::*operation)(), Startup<Result> startup)
    -> StartupStep
{
    return [service = std::move(service), operation, startup] { return run_startup(service, operation, startup); };
}

struct Dependencies
{
    shared_ptr<Lifecycle> lifecycle;
    // Run in order before async_main waits for shutdown.
    vector<StartupStep> startup;
};
} // namespace application
