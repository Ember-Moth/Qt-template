module;
#include <asio/post.hpp>
#include <asio/strand.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

module Template.Tasks;
import std;
import Template.Storage.Mmkv;

using std::string;
using std::string_view;
using std::shared_ptr;
using std::span;
using std::size_t;

namespace business {
namespace {
constexpr auto snapshotKey = "tasks.items";
using SnapshotResult = std::expected<storage::Strings, Error>;
auto storageFailure(const storage::Error &error, string_view context) -> Error
{
    const auto code = error.code == storage::ErrorCode::invalidFormat ? ErrorCode::invalidFormat : ErrorCode::storage;
    return {code, std::format("{}: {}", context, error.detail)};
}
auto decodeSnapshot(const storage::Strings &records) -> TaskResult
{
    if (records.size() % 3 != 0)
        return std::unexpected(Error{ErrorCode::invalidFormat,
            std::format("'{}' holds {} strings, not id/title/completed triples", snapshotKey, records.size())});
    auto tasks = Tasks{};
    tasks.reserve(records.size() / 3);
    for (auto index = size_t{0}; index < records.size(); index += 3) {
        const auto &completed = records[index + 2];
        if (completed != "0" && completed != "1")
            return std::unexpected(Error{ErrorCode::invalidFormat,
                std::format("'{}' record {} has completion value '{}', expected 0 or 1", snapshotKey, index / 3, completed)});
        tasks.push_back({.id = records[index], .title = records[index + 1], .completed = completed == "1"});
    }
    if (auto valid = validateTasks(tasks); !valid) return std::unexpected(withContext(valid.error(), snapshotKey));
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
auto notLoaded(string_view context) -> std::unexpected<Error>
{
    return std::unexpected(Error{ErrorCode::notReady, std::format("{}: the tasks are not loaded", context)});
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
        if (!executor)
            throw Exception({ErrorCode::missingDependency, "TaskService: an Asio executor is required"});
        if (!store)
            throw Exception({ErrorCode::missingDependency, "TaskService: an MMKV store is required"});
        executor = asio::make_strand(executor);
    }
    static auto keepAlive(shared_ptr<Impl> state, asio::awaitable<UpdateResult> operation) -> asio::awaitable<UpdateResult>
    {
        auto result = co_await std::move(operation);
        // The coroutine frame retains state even if the service handle is gone.
        static_cast<void>(state);
        co_return result;
    }
    auto enter(string_view operation) -> asio::awaitable<void>
    {
        const auto current = co_await asio::this_coro::executor;
        if (current != executor)
            throw Exception({ErrorCode::contractViolation,
                std::format("TaskService::{}: run task coroutines on TaskService::executor()", operation)});
        co_await asio::post(asio::use_awaitable);
    }
    auto snapshot(bool changed = false) const -> Update
    {
        return {.tasks = tasks, .ready = ready, .changed = changed};
    }
    auto commit(Tasks updated, string_view context) -> UpdateResult
    {
        const auto encoded = encodeSnapshot(updated);
        if (!encoded) return std::unexpected(withContext(encoded.error(), context));
        if (auto saved = store->setStrings(snapshotKey, *encoded); !saved)
            return std::unexpected(storageFailure(saved.error(), context));
        tasks = std::move(updated);
        return snapshot(true);
    }
    // Recoverable failures return Error; exceptions that reach here are unrecoverable.
    auto reload() -> asio::awaitable<UpdateResult>
    {
        co_await enter("reload");
        ready = false;
        const auto context = "load tasks";
        auto loaded = store->getStrings(snapshotKey);
        if (!loaded) co_return std::unexpected(storageFailure(loaded.error(), context));
        auto decoded = decodeSnapshot(loaded->value_or(storage::Strings{}));
        if (!decoded) co_return std::unexpected(withContext(decoded.error(), context));
        tasks = std::move(*decoded);
        ready = true;
        co_return snapshot(true);
    }
    auto addTask(string title) -> asio::awaitable<UpdateResult>
    {
        co_await enter("addTask");
        const auto context = "add task";
        if (!ready) co_return notLoaded(context);
        auto normalized = normalizeTitle(title);
        if (!normalized) co_return std::unexpected(withContext(normalized.error(), context));
        auto updated = tasks;
        updated.push_back({.id = createTaskId(), .title = std::move(*normalized)});
        co_return commit(std::move(updated), context);
    }
    auto setTaskCompleted(string id, bool completed) -> asio::awaitable<UpdateResult>
    {
        co_await enter("setTaskCompleted");
        const auto context = std::format("{} task '{}'", completed ? "complete" : "reopen", id);
        if (!ready) co_return notLoaded(context);
        auto updated = tasks;
        auto found = std::ranges::find(updated, id, &Task::id);
        if (found == updated.end())
            co_return std::unexpected(Error{ErrorCode::notFound, std::format("{}: no task has this id", context)});
        if (found->completed == completed) co_return snapshot();
        found->completed = completed;
        co_return commit(std::move(updated), context);
    }
    auto removeTask(string id) -> asio::awaitable<UpdateResult>
    {
        co_await enter("removeTask");
        const auto context = std::format("remove task '{}'", id);
        if (!ready) co_return notLoaded(context);
        auto updated = tasks;
        if (std::erase_if(updated, [&id](const auto &task) { return task.id == id; }) == 0)
            co_return std::unexpected(Error{ErrorCode::notFound, std::format("{}: no task has this id", context)});
        co_return commit(std::move(updated), context);
    }
};

TaskService::TaskService(asio::any_io_executor executor, shared_ptr<storage::MmkvStore> store)
    : m_impl(std::make_shared<Impl>(std::move(executor), std::move(store))) {}
TaskService::~TaskService() = default;
auto TaskService::executor() const -> asio::any_io_executor { return m_impl->executor; }
// Capture shared state when creating the coroutine, before the service is destroyed.
auto TaskService::reload() -> asio::awaitable<UpdateResult> { return Impl::keepAlive(m_impl, m_impl->reload()); }
auto TaskService::addTask(string title) -> asio::awaitable<UpdateResult>
{
    return Impl::keepAlive(m_impl, m_impl->addTask(std::move(title)));
}
auto TaskService::setTaskCompleted(string id, bool completed) -> asio::awaitable<UpdateResult>
{
    return Impl::keepAlive(m_impl, m_impl->setTaskCompleted(std::move(id), completed));
}
auto TaskService::removeTask(string id) -> asio::awaitable<UpdateResult>
{
    return Impl::keepAlive(m_impl, m_impl->removeTask(std::move(id)));
}
} // namespace business
