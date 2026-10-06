module;
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

export module Template.App.AsyncMain;
import std;
import Template.Tasks;

using std::shared_ptr;

export namespace application {
struct Dependencies;
auto async_main(Dependencies dependencies) -> asio::awaitable<void>;

// One application run: one startup result and a latched shutdown notification.
class Lifecycle
{
public:
    explicit Lifecycle(asio::any_io_executor executor);
    ~Lifecycle();
    Lifecycle(const Lifecycle &) = delete;
    Lifecycle &operator=(const Lifecycle &) = delete;

    auto startup() -> asio::awaitable<business::Update>;
    void requestStop();

private:
    struct Impl;
    shared_ptr<Impl> m_impl;
    friend auto async_main(Dependencies dependencies) -> asio::awaitable<void>;
};

struct Dependencies
{
    shared_ptr<business::TaskService> tasks;
    shared_ptr<Lifecycle> lifecycle;
};
} // namespace application
