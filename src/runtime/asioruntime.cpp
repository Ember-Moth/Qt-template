module;
#include <asio/executor_work_guard.hpp>
#include <asio/io_context.hpp>

module Template.Runtime.Asio;
import std;

using std::jthread;

namespace runtime {
struct AsioRuntime::Impl
{
    asio::io_context context;
    using WorkGuard = decltype(asio::make_work_guard(context));
    WorkGuard work{asio::make_work_guard(context)};
    jthread worker{[this] { context.run(); }};

    ~Impl() { finish(); }
    void finish()
    {
        work.reset();
        if (worker.joinable())
            worker.join();
    }
};

AsioRuntime::AsioRuntime() : m_impl(std::make_unique<Impl>()) {}
AsioRuntime::~AsioRuntime() = default;
auto AsioRuntime::executor() const -> asio::any_io_executor { return m_impl->context.get_executor(); }
void AsioRuntime::finish() { m_impl->finish(); }
} // namespace runtime
