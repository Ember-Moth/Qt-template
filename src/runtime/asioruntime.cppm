module;
// io_context is only forward-declared here; units that use the loop include <asio/io_context.hpp>.
#include "runtime/task.h"

export module Template.Runtime.Asio;
import std;

using std::unique_ptr;
using asio::io_context;

export namespace runtime {
// Concrete Asio infrastructure: one event loop on one worker thread, with application-owned lifetime.
class AsioRuntime
{
public:
    AsioRuntime();
    ~AsioRuntime();
    AsioRuntime(const AsioRuntime &) = delete;
    AsioRuntime &operator=(const AsioRuntime &) = delete;

    auto executor() const -> Executor;
    // The event loop itself, for libraries built on io_context such as cofetch through
    // network::HttpClient. Its handlers all run on the single worker thread.
    auto context() const -> io_context &;
    // Call on the owning thread after submissions stop; drain accepted work.
    void finish();

private:
    struct Impl;
    unique_ptr<Impl> m_impl;
};
} // namespace runtime
