module;
#include "runtime/task.h"
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

module Template.Runtime.Asio;
import std;

using std::jthread;
using std::make_unique;
using asio::io_context;
using asio::make_work_guard;

namespace runtime {
struct AsioRuntime::Impl
{
    io_context context;
    using WorkGuard = decltype(asio::make_work_guard(context));
    WorkGuard work{make_work_guard(context)};
    jthread worker{[this] { context.run(); }};

    ~Impl() { finish(); }
    void finish()
    {
        work.reset();
        if (worker.joinable())
            worker.join();
    }
};

AsioRuntime::AsioRuntime() : m_impl(make_unique<Impl>()) {}
AsioRuntime::~AsioRuntime() = default;
auto AsioRuntime::executor() const -> Executor { return m_impl->context.get_executor(); }
auto AsioRuntime::context() const -> io_context & { return m_impl->context; }
void AsioRuntime::finish() { m_impl->finish(); }
} // namespace runtime
