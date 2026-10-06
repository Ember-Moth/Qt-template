module;
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

export module Template.Tasks;
import std;
export import Template.Models;
import Template.Storage.Mmkv;

using std::string;
using std::optional;
using std::shared_ptr;

export namespace business {
struct Update
{
    Tasks tasks;
    bool ready = false;
    bool changed = false;
    optional<Error> error;
};

class TaskService
{
public:
    TaskService(asio::any_io_executor executor, shared_ptr<storage::MmkvStore> store);
    ~TaskService();
    TaskService(const TaskService &) = delete;
    TaskService &operator=(const TaskService &) = delete;

    auto executor() const -> asio::any_io_executor;
    auto reload() -> asio::awaitable<Update>;
    auto addTask(string title) -> asio::awaitable<Update>;
    auto setTaskCompleted(string id, bool completed) -> asio::awaitable<Update>;
    auto removeTask(string id) -> asio::awaitable<Update>;

private:
    struct Impl;
    shared_ptr<Impl> m_impl;
};
} // namespace business
