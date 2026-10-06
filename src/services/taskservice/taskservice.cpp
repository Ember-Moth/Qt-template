module;
#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

module Template.Tasks;
import std;
import Template.Storage.Mmkv;

using std::string;
using std::optional;
using std::shared_ptr;
using std::exception;
using std::invalid_argument;
using std::logic_error;
using std::span;
using std::size_t;

namespace business {
namespace {
constexpr auto snapshotKey = "tasks.items";
using SnapshotResult = std::expected<storage::Strings, Error>;
auto storageFailure(const storage::Error &error) -> Error
{
    const auto code = error.code == storage::ErrorCode::invalidFormat ? ErrorCode::invalidFormat : ErrorCode::storage;
    return {code, error.detail};
}
auto decodeSnapshot(const storage::Strings &records) -> TaskResult
{
    if (records.size() % 3 != 0)
        return std::unexpected(Error{ErrorCode::invalidFormat, "Invalid task record count."});
    auto tasks = Tasks{};
    tasks.reserve(records.size() / 3);
    for (auto index = size_t{0}; index < records.size(); index += 3) {
        const auto &completed = records[index + 2];
        if (completed != "0" && completed != "1")
            return std::unexpected(Error{ErrorCode::invalidFormat, "Invalid task completion value."});
        tasks.push_back({.id = records[index], .title = records[index + 1], .completed = completed == "1"});
    }
    if (auto valid = validateTasks(tasks); !valid) return std::unexpected(valid.error());
    return tasks;
}
auto encodeSnapshot(span<const Task> tasks) -> SnapshotResult
{
    if (auto valid = validateTasks(tasks); !valid) return std::unexpected(valid.error());
    auto records = storage::Strings{};
    records.reserve(tasks.size() * 3);
    for (const auto &task : tasks) {
        records.push_back(task.id);
        records.push_back(task.title);
        records.emplace_back(task.completed ? "1" : "0");
    }
    return records;
}
} // namespace

struct TaskService::Impl
{
    shared_ptr<storage::MmkvStore> store;
    asio::any_io_executor executor;
    Tasks tasks;
    bool ready = false;
    Impl(asio::any_io_executor target, shared_ptr<storage::MmkvStore> source)
        : store(std::move(source)), executor(std::move(target))
    {
        if (!executor || !store)
            throw invalid_argument("An Asio executor and an MMKV store are required.");
        executor = asio::make_strand(executor);
    }
    static auto keepAlive(shared_ptr<Impl> state, asio::awaitable<Update> operation) -> asio::awaitable<Update>
    {
        auto result = co_await std::move(operation);
        // The coroutine frame retains state even if the service handle is gone.
        static_cast<void>(state);
        co_return result;
    }
    auto enter() -> asio::awaitable<void>
    {
        const auto current = co_await asio::this_coro::executor;
        if (current != executor)
            throw logic_error("Run task coroutines on TaskService::executor().");
        co_await asio::post(asio::use_awaitable);
    }
    auto snapshot(bool changed = false, optional<Error> error = {}) const -> Update
    {
        return {.tasks = tasks, .ready = ready, .changed = changed, .error = std::move(error)};
    }
    auto commit(Tasks updated) -> Update
    {
        const auto encoded = encodeSnapshot(updated);
        if (!encoded) return snapshot(false, encoded.error());
        if (auto saved = store->setStrings(snapshotKey, *encoded); !saved)
            return snapshot(false, storageFailure(saved.error()));
        tasks = std::move(updated);
        return snapshot(true);
    }
    auto reload() -> asio::awaitable<Update>
    {
        co_await enter();
        ready = false;
        try {
            auto loaded = store->getStrings(snapshotKey);
            if (!loaded) co_return snapshot(false, storageFailure(loaded.error()));
            auto decoded = decodeSnapshot(loaded->value_or(storage::Strings{}));
            if (!decoded) co_return snapshot(false, decoded.error());
            tasks = std::move(*decoded);
            ready = true;
            co_return snapshot(true);
        } catch (const exception &error) {
            co_return snapshot(false, Error{ErrorCode::internal, error.what()});
        }
    }
    auto addTask(string title) -> asio::awaitable<Update>
    {
        co_await enter();
        if (!ready) co_return snapshot(false, Error{ErrorCode::notReady, {}});
        try {
            auto normalized = normalizeTitle(title);
            if (!normalized) co_return snapshot(false, normalized.error());
            auto updated = tasks;
            updated.push_back({.id = createTaskId(), .title = std::move(*normalized)});
            co_return commit(std::move(updated));
        } catch (const exception &error) {
            co_return snapshot(false, Error{ErrorCode::internal, error.what()});
        }
    }
    auto setTaskCompleted(string id, bool completed) -> asio::awaitable<Update>
    {
        co_await enter();
        if (!ready) co_return snapshot(false, Error{ErrorCode::notReady, {}});
        try {
            auto updated = tasks;
            auto found = std::ranges::find(updated, id, &Task::id);
            if (found == updated.end()) co_return snapshot(false, Error{ErrorCode::notFound, {}});
            if (found->completed == completed) co_return snapshot();
            found->completed = completed;
            co_return commit(std::move(updated));
        } catch (const exception &error) {
            co_return snapshot(false, Error{ErrorCode::internal, error.what()});
        }
    }
    auto removeTask(string id) -> asio::awaitable<Update>
    {
        co_await enter();
        if (!ready) co_return snapshot(false, Error{ErrorCode::notReady, {}});
        try {
            auto updated = tasks;
            if (std::erase_if(updated, [&id](const auto &task) { return task.id == id; }) == 0)
                co_return snapshot(false, Error{ErrorCode::notFound, {}});
            co_return commit(std::move(updated));
        } catch (const exception &error) {
            co_return snapshot(false, Error{ErrorCode::internal, error.what()});
        }
    }
};

TaskService::TaskService(asio::any_io_executor executor, shared_ptr<storage::MmkvStore> store)
    : m_impl(std::make_shared<Impl>(std::move(executor), std::move(store))) {}
TaskService::~TaskService() = default;
auto TaskService::executor() const -> asio::any_io_executor { return m_impl->executor; }
// Capture shared state when creating the coroutine, before the service is destroyed.
auto TaskService::reload() -> asio::awaitable<Update> { return Impl::keepAlive(m_impl, m_impl->reload()); }
auto TaskService::addTask(string title) -> asio::awaitable<Update>
{
    return Impl::keepAlive(m_impl, m_impl->addTask(std::move(title)));
}
auto TaskService::setTaskCompleted(string id, bool completed) -> asio::awaitable<Update>
{
    return Impl::keepAlive(m_impl, m_impl->setTaskCompleted(std::move(id), completed));
}
auto TaskService::removeTask(string id) -> asio::awaitable<Update>
{
    return Impl::keepAlive(m_impl, m_impl->removeTask(std::move(id)));
}
} // namespace business
