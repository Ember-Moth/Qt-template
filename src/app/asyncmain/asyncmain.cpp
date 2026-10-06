module;
#include <asio/co_spawn.hpp>
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/use_awaitable.hpp>

module Template.App.AsyncMain;
import std;

using std::shared_ptr;
using std::atomic_bool;
using std::invalid_argument;
using std::logic_error;

namespace application {
struct Lifecycle::Impl
{
    using Startup = asio::experimental::concurrent_channel<void(asio::error_code, business::Update)>;
    using Shutdown = asio::experimental::concurrent_channel<void(asio::error_code)>;
    Startup started;
    Shutdown stopping;
    atomic_bool stopRequested = false;
    atomic_bool running = false;

    explicit Impl(asio::any_io_executor executor) : started(executor, 1), stopping(executor, 1) {}
    static auto receive(shared_ptr<Impl> state) -> asio::awaitable<business::Update>
    {
        co_return co_await state->started.async_receive(asio::use_awaitable);
    }
};

Lifecycle::Lifecycle(asio::any_io_executor executor)
{
    if (!executor) throw invalid_argument("An application executor is required.");
    m_impl = std::make_shared<Impl>(std::move(executor));
}
Lifecycle::~Lifecycle() = default;
auto Lifecycle::startup() -> asio::awaitable<business::Update> { return Impl::receive(m_impl); }
void Lifecycle::requestStop()
{
    if (m_impl->stopRequested.exchange(true)) return;
    // Release the startup receiver even if shutdown happens before async_main starts.
    m_impl->started.close();
    m_impl->stopping.try_send(asio::error_code{});
}

auto async_main(Dependencies dependencies) -> asio::awaitable<void>
{
    if (!dependencies.lifecycle)
        throw invalid_argument("An application lifecycle is required.");
    const auto state = dependencies.lifecycle->m_impl;
    if (state->running.exchange(true))
        throw logic_error("Run async_main once per application lifecycle.");
    try {
        if (!dependencies.tasks)
            throw invalid_argument("Application services are required.");
        if (state->stopRequested.load()) co_return;

        // Every service keeps its own strand, even when called by the root coroutine.
        auto loaded = co_await asio::co_spawn(dependencies.tasks->executor(),
            dependencies.tasks->reload(), asio::use_awaitable);
        if (!state->started.try_send(asio::error_code{}, std::move(loaded)) && !state->stopRequested.load())
            throw logic_error("The application startup result could not be delivered.");

        co_await state->stopping.async_receive(asio::use_awaitable);
        // Cancel and await long-lived business I/O here when adding it to the template.
        // The task sample has finite operations; the runtime drains those before RAII teardown.
    } catch (...) {
        state->started.close();
        throw;
    }
}
} // namespace application
