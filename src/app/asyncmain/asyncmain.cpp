module;
#include "runtime/task.h"
#include <asio/experimental/concurrent_channel.hpp>
#include <asio/use_awaitable.hpp>

module Template.App.AsyncMain;
import std;

using std::function;
using std::vector;
using std::mutex;
using std::scoped_lock;
using std::atomic_bool;
using std::size_t;
using std::format;
using std::make_shared;
using asio::use_awaitable;
using asio::error_code;
using asio::experimental::concurrent_channel;
using runtime::Task;
using runtime::Executor;

namespace application {
struct Lifecycle::Impl
{
    using Shutdown = concurrent_channel<void(error_code)>;
    Executor executor;
    Shutdown stopping;
    atomic_bool stopRequested = false;
    atomic_bool running = false;
    mutex gate;
    vector<function<void()>> startups;

    explicit Impl(Executor target) : executor(target), stopping(target, 1) {}
    void cancelStartups()
    {
        auto cancels = vector<function<void()>>{};
        {
            const auto lock = scoped_lock(gate);
            cancels = startups;
        }
        for (const auto &cancel : cancels)
            cancel();
    }
};

Lifecycle::Lifecycle(Executor executor)
{
    if (!executor) throw Exception({ErrorCode::missingDependency, "Lifecycle: an application executor is required"});
    m_impl = make_shared<Impl>(std::move(executor));
}
Lifecycle::~Lifecycle() = default;
auto Lifecycle::executor() const -> Executor { return m_impl->executor; }
void Lifecycle::track(function<void()> cancel)
{
    {
        const auto lock = scoped_lock(m_impl->gate);
        m_impl->startups.push_back(cancel);
    }
    if (m_impl->stopRequested.load()) cancel();
}
void Lifecycle::requestStop()
{
    if (m_impl->stopRequested.exchange(true)) return;
    // Release startup consumers even if shutdown happens before async_main starts.
    m_impl->cancelStartups();
    m_impl->stopping.try_send(error_code{});
}

auto async_main(Dependencies dependencies) -> Task<>
{
    if (!dependencies.lifecycle)
        throw Exception({ErrorCode::missingDependency, "async_main: an application lifecycle is required"});
    const auto state = dependencies.lifecycle->m_impl;
    if (state->running.exchange(true))
        throw Exception({ErrorCode::contractViolation, "async_main: this lifecycle is already running"});
    try {
        for (auto index = size_t{0}; index < dependencies.startup.size(); ++index) {
            if (state->stopRequested.load()) co_return;
            if (!co_await dependencies.startup[index]() && !state->stopRequested.load())
                throw Exception({ErrorCode::contractViolation, format(
                    "async_main: startup step {} could not deliver its result; register each Startup for one step", index)});
        }

        co_await state->stopping.async_receive(use_awaitable);
        // Cancel and await long-lived business I/O here when adding it to the template.
        // The task sample has finite operations; the runtime drains those before RAII teardown.
    } catch (...) {
        state->cancelStartups();
        throw;
    }
}
} // namespace application
