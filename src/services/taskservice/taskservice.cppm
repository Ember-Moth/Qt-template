module;
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

export module Template.Tasks;
import std;
export import Template.Models;
import Template.Storage.Mmkv;

using std::string;
using std::expected;
using std::shared_ptr;

export namespace business {
// The committed tasks after an operation; changed is false when it left them as they were.
struct Update
{
    Tasks tasks;
    bool ready = false;
    bool changed = false;
};
// A failed operation keeps the committed state. Construction failures and calls from another
// executor throw Exception instead.
using UpdateResult = expected<Update, Error>;

class TaskService
{
public:
    TaskService(asio::any_io_executor executor, shared_ptr<storage::MmkvStore> store);
    ~TaskService();
    TaskService(const TaskService &) = delete;
    TaskService &operator=(const TaskService &) = delete;

    auto executor() const -> asio::any_io_executor;
    auto reload() -> asio::awaitable<UpdateResult>;
    auto addTask(string title) -> asio::awaitable<UpdateResult>;
    auto setTaskCompleted(string id, bool completed) -> asio::awaitable<UpdateResult>;
    auto removeTask(string id) -> asio::awaitable<UpdateResult>;

private:
    struct Impl;
    shared_ptr<Impl> m_impl;
};
} // namespace business
