module;
#include <asio/any_io_executor.hpp>

export module Template.Runtime.Asio;
import std;

using std::unique_ptr;

export namespace runtime {
// Concrete Asio infrastructure: one event loop with application-owned lifetime.
class AsioRuntime
{
public:
    AsioRuntime();
    ~AsioRuntime();
    AsioRuntime(const AsioRuntime &) = delete;
    AsioRuntime &operator=(const AsioRuntime &) = delete;

    auto executor() const -> asio::any_io_executor;
    // Call on the owning thread after submissions stop; drain accepted work.
    void finish();

private:
    struct Impl;
    unique_ptr<Impl> m_impl;
};
} // namespace runtime
