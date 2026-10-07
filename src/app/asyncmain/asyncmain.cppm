module;
#include <asio/any_io_executor.hpp>
#include <asio/as_tuple.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/use_awaitable.hpp>

export module Template.App.AsyncMain;
import std;

using std::shared_ptr;
using std::function;
using std::vector;
using std::optional;
using std::string;

namespace application::detail {
// Importers instantiate Asio channel templates that turn channel_errors into error_code through
// std::is_error_code_enum and ADL make_error_code. Reduced BMIs keep only global-fragment
// declarations the module references, so reference both here.
inline auto channelError(asio::experimental::error::channel_errors code) -> asio::error_code
{
    return code;
}
} // namespace application::detail

export namespace application {
// Application wiring failures; all are thrown as Exception because the run cannot continue.
enum class ErrorCode { missingDependency, contractViolation };
// detail reads "<context>: <reason>".
struct Error
{
    ErrorCode code;
    string detail;
};
class Exception : public std::exception
{
public:
    explicit Exception(Error error) : m_error(std::move(error)) {}
    auto error() const noexcept -> const Error & { return m_error; }
    auto what() const noexcept -> const char * override { return m_error.detail.c_str(); }

private:
    Error m_error;
};

struct Dependencies;
auto async_main(Dependencies dependencies) -> asio::awaitable<void>;

// A one-shot startup result for a single consumer; stopping or a startup failure cancels it.
// Cancellation is an expected outcome of shutting down, so it is an empty result, not an error.
template <class Result>
class Startup
{
public:
    explicit Startup(asio::any_io_executor executor) : m_channel(std::make_shared<Channel>(std::move(executor), 1)) {}

    // Completes with the delivered result, or with nullopt once cancelled.
    auto result() const -> asio::awaitable<optional<Result>> { return receive(m_channel); }
    auto deliver(Result result) const -> bool { return m_channel->try_send(asio::error_code{}, std::move(result)); }
    void cancel() const { m_channel->close(); }

private:
    using Channel = asio::experimental::concurrent_channel<void(asio::error_code, Result)>;
    static auto receive(shared_ptr<Channel> channel) -> asio::awaitable<optional<Result>>
    {
        auto [error, result] = co_await channel->async_receive(asio::as_tuple(asio::use_awaitable));
        if (error) co_return std::nullopt;
        co_return optional<Result>{std::move(result)};
    }
    shared_ptr<Channel> m_channel;
};

// One application run: the startup results registered for it and a latched shutdown notification.
class Lifecycle
{
public:
    explicit Lifecycle(asio::any_io_executor executor);
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
    auto executor() const -> asio::any_io_executor;
    void track(function<void()> cancel);
    friend auto async_main(Dependencies dependencies) -> asio::awaitable<void>;
};

// Runs one service operation at startup and reports whether its consumer received the result.
using StartupStep = function<asio::awaitable<bool>()>;

template <class Service, class Result>
auto run_startup(shared_ptr<Service> service, asio::awaitable<Result> (Service::*operation)(), Startup<Result> startup)
    -> asio::awaitable<bool>
{
    if (!service)
        throw Exception({ErrorCode::missingDependency, "async_main: a startup step has no service"});
    // Every service keeps its own strand, even when called by the root coroutine.
    auto result = co_await asio::co_spawn(service->executor(), ((*service).*operation)(), asio::use_awaitable);
    co_return startup.deliver(std::move(result));
}

template <class Service, class Result>
auto startup_step(shared_ptr<Service> service, asio::awaitable<Result> (Service::*operation)(), Startup<Result> startup)
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
