module;
#include "runtime/task.h"
#include <asio/co_spawn.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>

module Template.Tasks;
import std;
import Template.Storage.Mmkv;

using std::string;
// Same standard type; avoid clangd merging duplicate aliases from Asio headers and the std BMI.
using string_view = std::basic_string_view<char>;
using std::shared_ptr;
using std::span;
using std::size_t;
using std::format;
using std::make_shared;
using std::erase_if;
using std::ranges::find;
using asio::co_spawn;
using asio::make_strand;
using asio::use_awaitable;
using runtime::Task;
using runtime::Executor;
using storage::MmkvStore;
using storage::Strings;
using errors::Ok;
using errors::Err;

// Rust's impl From<storage::Error> for business::Error: a storage failure keeps its detail, and `?`
// converts it on the way out.
template <>
struct errors::From<business::ErrorCode, storage::ErrorCode>
{
    static auto code(storage::ErrorCode source) -> business::ErrorCode
    {
        return source == storage::ErrorCode::invalidFormat ? business::ErrorCode::invalidFormat
                                                           : business::ErrorCode::storage;
    }
};

namespace business {
namespace {
constexpr auto snapshotKey = "tasks.items";

auto decodeSnapshot(const Strings &records) -> TaskResult
{
    if (records.size() % 3 != 0)
        co_return Err(ErrorCode::invalidFormat, "'{}' holds {} strings, not id/title/completed triples",
            snapshotKey, records.size());
    auto tasks = Tasks{};
    tasks.reserve(records.size() / 3);
    for (auto index = size_t{0}; index < records.size(); index += 3) {
        const auto &completed = records[index + 2];
        if (completed != "0" && completed != "1")
            co_return Err(ErrorCode::invalidFormat, "'{}' record {} has completion value '{}', expected 0 or 1",
                snapshotKey, index / 3, completed);
        tasks.push_back({.id = records[index], .title = records[index + 1], .completed = completed == "1"});
    }
    co_await validateTasks(tasks).context(snapshotKey);
    co_return tasks;
}
auto encodeSnapshot(span<const TaskRecord> tasks) -> Result<Strings>
{
    co_await validateTasks(tasks);
    auto records = Strings{};
    records.reserve(tasks.size() * 3);
    for (const auto &task : tasks) {
        records.push_back(task.id);
        records.push_back(task.title);
        records.emplace_back(task.completed ? "1" : "0");
    }
    co_return records;
}
} // namespace

// The operations are synchronous Result functions that run on the strand; co_await in them is `?`.
struct TaskService::Impl
{
    shared_ptr<MmkvStore> store;
    // Business state below is touched only on this strand.
    Executor strand;
    Tasks tasks;
    bool ready = false;
    Impl(Executor executor, shared_ptr<MmkvStore> source) : store(std::move(source))
    {
        if (!executor)
            throw Exception({ErrorCode::missingDependency, "TaskService: an Asio executor is required"});
        if (!store)
            throw Exception({ErrorCode::missingDependency, "TaskService: an MMKV store is required"});
        strand = make_strand(std::move(executor));
    }
    // Runs operation on the strand and resumes the caller on its own executor. The coroutine holds the
    // shared state, so the operation completes even if the service handle is gone first.
    template <class Operation>
    static auto onStrand(shared_ptr<Impl> self, Operation operation) -> Task<UpdateResult>
    {
        const auto strand = self->strand;
        co_return co_await co_spawn(strand, [self = std::move(self), operation = std::move(operation)]() mutable
            -> Task<UpdateResult> { co_return operation(*self); }, use_awaitable);
    }

    auto snapshot(bool changed = false) const -> Update
    {
        return {.tasks = tasks, .ready = ready, .changed = changed};
    }
    auto requireLoaded(string_view context) const -> Result<void>
    {
        if (!ready) return Err(ErrorCode::notReady, "{}: the tasks are not loaded", context);
        return Ok();
    }
    // Saves first and keeps the committed tasks when saving fails.
    auto commit(Tasks updated, string_view context) -> UpdateResult
    {
        const auto records = co_await encodeSnapshot(updated).context(context);
        co_await store->setStrings(snapshotKey, records).context(context);
        tasks = std::move(updated);
        co_return snapshot(true);
    }
    auto reload() -> UpdateResult
    {
        ready = false;
        const auto context = "load tasks";
        const auto records = co_await store->getStrings(snapshotKey).context(context);
        tasks = co_await decodeSnapshot(records.value_or(Strings{})).context(context);
        ready = true;
        co_return snapshot(true);
    }
    auto addTask(string title) -> UpdateResult
    {
        const auto context = "add task";
        co_await requireLoaded(context);
        auto normalized = co_await normalizeTitle(title).context(context);
        auto updated = tasks;
        updated.push_back({.id = createTaskId(), .title = std::move(normalized)});
        co_return commit(std::move(updated), context);
    }
    auto setTaskCompleted(string_view id, bool completed) -> UpdateResult
    {
        const auto context = format("{} task '{}'", completed ? "complete" : "reopen", id);
        co_await requireLoaded(context);
        auto updated = tasks;
        auto found = find(updated, id, &TaskRecord::id);
        if (found == updated.end()) co_return Err(ErrorCode::notFound, "{}: no task has this id", context);
        if (found->completed == completed) co_return snapshot();
        found->completed = completed;
        co_return commit(std::move(updated), context);
    }
    auto removeTask(string_view id) -> UpdateResult
    {
        const auto context = format("remove task '{}'", id);
        co_await requireLoaded(context);
        auto updated = tasks;
        if (erase_if(updated, [id](const auto &task) { return task.id == id; }) == 0)
            co_return Err(ErrorCode::notFound, "{}: no task has this id", context);
        co_return commit(std::move(updated), context);
    }
};

TaskService::TaskService(Executor executor, shared_ptr<MmkvStore> store)
    : m_impl(make_shared<Impl>(std::move(executor), std::move(store))) {}
TaskService::~TaskService() = default;
auto TaskService::reload() -> Task<UpdateResult>
{
    return Impl::onStrand(m_impl, [](Impl &impl) { return impl.reload(); });
}
auto TaskService::addTask(string title) -> Task<UpdateResult>
{
    return Impl::onStrand(m_impl, [title = std::move(title)](Impl &impl) { return impl.addTask(title); });
}
auto TaskService::setTaskCompleted(string id, bool completed) -> Task<UpdateResult>
{
    return Impl::onStrand(m_impl, [id = std::move(id), completed](Impl &impl) { return impl.setTaskCompleted(id, completed); });
}
auto TaskService::removeTask(string id) -> Task<UpdateResult>
{
    return Impl::onStrand(m_impl, [id = std::move(id)](Impl &impl) { return impl.removeTask(id); });
}
} // namespace business
