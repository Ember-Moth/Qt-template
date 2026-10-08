#include "runtime/task.h"
#include <asio/co_spawn.hpp>
#include <asio/use_future.hpp>

import std;
import Template.App.AsyncMain;
import Template.Runtime.Asio;
import Template.Storage.Mmkv;
import Template.Tasks;

using std::shared_ptr;
using std::source_location;
using std::runtime_error;
using std::future_status;

namespace {
void require(bool condition, source_location where = source_location::current())
{
    if (!condition)
        throw runtime_error(std::format("{}:{} check failed", where.file_name(), where.line()));
}
struct TemporaryDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("qt-template-entry-" + business::createTaskId());
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { std::error_code ignored; std::filesystem::remove_all(path, ignored); }
};
struct Fixture
{
    TemporaryDirectory directory;
    runtime::AsioRuntime runtime;
    shared_ptr<storage::MmkvStore> store = std::make_shared<storage::MmkvStore>(directory.path / "mmkv");
    shared_ptr<business::TaskService> tasks = std::make_shared<business::TaskService>(runtime.executor(), store);
    shared_ptr<application::Lifecycle> lifecycle = std::make_shared<application::Lifecycle>(runtime.executor());
    application::Startup<business::UpdateResult> startup = lifecycle->startup<business::UpdateResult>();
    auto run(std::vector<application::StartupStep> steps)
    {
        return asio::co_spawn(runtime.executor(), application::async_main({lifecycle, std::move(steps)}), asio::use_future);
    }
    auto run() { return run({application::startup_step(tasks, &business::TaskService::reload, startup)}); }
    template <class Result> auto received(const application::Startup<Result> &result)
    {
        return asio::co_spawn(runtime.executor(), result.result(), asio::use_future);
    }
    auto received() { return received(startup); }
    ~Fixture() { lifecycle->requestStop(); runtime.finish(); }
};
template <class T> auto receive(std::future<T> &future) -> T
{
    require(future.wait_for(std::chrono::seconds(5)) == future_status::ready);
    return future.get();
}
// A delivered startup result that succeeded.
auto loaded(std::future<std::optional<business::UpdateResult>> &future) -> business::Update
{
    auto result = receive(future);
    require(result && result->has_value());
    return **result;
}
// Cancellation is an empty result, not an exception.
template <class T> void cancelled(std::future<std::optional<T>> &future)
{
    require(!receive(future).has_value());
}
template <class Run> void throwsApplicationError(Run run, application::ErrorCode code, std::string_view context)
{
    try {
        run();
        require(false);
    } catch (const application::Exception &error) {
        require(error.error().code == code && error.error().detail.find(context) != std::string::npos);
    }
}

void startupAndShutdown()
{
    Fixture fixture;
    const auto records = storage::Strings{"seed", "Loaded by async_main", "0"};
    require(fixture.store->setStrings("tasks.items", records).has_value());
    auto startup = fixture.received();
    auto entry = fixture.run();
    const auto update = loaded(startup);
    require(update.ready && update.changed);
    require(update.tasks.size() == 1 && update.tasks.at(0).title == "Loaded by async_main");
    require(entry.wait_for(std::chrono::seconds(0)) == future_status::timeout);
    auto add = asio::co_spawn(fixture.runtime.executor(), fixture.tasks->addTask("While running"), asio::use_future);
    require(receive(add)->tasks.size() == 2);
    fixture.lifecycle->requestStop();
    fixture.lifecycle->requestStop();
    receive(entry);
    // Shutdown completes without a Qt event loop, then drains remaining accepted work.
    fixture.runtime.finish();
}

void stopBeforeStartup()
{
    Fixture fixture;
    fixture.lifecycle->requestStop();
    auto startup = fixture.received();
    auto entry = fixture.run();
    receive(entry);
    cancelled(startup);
    require(!std::filesystem::exists(fixture.directory.path / "mmkv"));
    // A result registered after stopping is released at once.
    auto late = fixture.received(fixture.lifecycle->startup<business::UpdateResult>());
    cancelled(late);
}

void startupFailureAndRecovery()
{
    Fixture fixture;
    const auto path = fixture.directory.path / "mmkv";
    { auto blocker = std::ofstream{path}; blocker << "blocked"; }
    auto startup = fixture.received();
    auto entry = fixture.run();
    // A failed load is a delivered result carrying the business error, not a cancellation.
    const auto failed = receive(startup);
    require(failed && !failed->has_value() && failed->error().code == business::ErrorCode::storage);
    require(entry.wait_for(std::chrono::seconds(0)) == future_status::timeout);
    std::filesystem::remove(path);
    auto retry = asio::co_spawn(fixture.runtime.executor(), fixture.tasks->reload(), asio::use_future);
    require(receive(retry)->ready);
    fixture.lifecycle->requestStop();
    receive(entry);
}

void immediateShutdown()
{
    for (auto index = 0; index < 30; ++index) {
        Fixture fixture;
        auto startup = fixture.received();
        auto entry = fixture.run();
        fixture.lifecycle->requestStop();
        receive(entry);
        // Either delivered before the stop or cancelled by it; neither throws.
        static_cast<void>(receive(startup));
        fixture.runtime.finish();
    }
}

void unexpectedFailureReleasesStartup()
{
    Fixture fixture;
    fixture.tasks.reset();
    auto startup = fixture.received();
    auto entry = fixture.run();
    throwsApplicationError([&] { receive(entry); }, application::ErrorCode::missingDependency, "has no service");
    cancelled(startup);
}

void independentStartupSteps()
{
    Fixture fixture;
    // A second feature brings its own service and startup result; async_main stays unchanged.
    auto notes = std::make_shared<business::TaskService>(fixture.runtime.executor(),
        std::make_shared<storage::MmkvStore>(fixture.directory.path / "mmkv", "notes"));
    const auto notesStartup = fixture.lifecycle->startup<business::UpdateResult>();
    require(fixture.store->setStrings("tasks.items", storage::Strings{"task", "First feature", "0"}).has_value());
    auto first = fixture.received();
    auto second = fixture.received(notesStartup);
    auto entry = fixture.run({application::startup_step(fixture.tasks, &business::TaskService::reload, fixture.startup),
        application::startup_step(notes, &business::TaskService::reload, notesStartup)});
    require(loaded(first).tasks.at(0).title == "First feature");
    const auto notesLoaded = loaded(second);
    require(notesLoaded.ready && notesLoaded.tasks.empty());
    require(entry.wait_for(std::chrono::seconds(0)) == future_status::timeout);
    fixture.lifecycle->requestStop();
    receive(entry);
}

void laterStartupFailureReleasesRemainingConsumers()
{
    Fixture fixture;
    const auto missing = fixture.lifecycle->startup<business::UpdateResult>();
    auto first = fixture.received();
    auto second = fixture.received(missing);
    auto entry = fixture.run({application::startup_step(fixture.tasks, &business::TaskService::reload, fixture.startup),
        application::startup_step(shared_ptr<business::TaskService>{}, &business::TaskService::reload, missing)});
    require(loaded(first).ready);
    throwsApplicationError([&] { receive(entry); }, application::ErrorCode::missingDependency, "has no service");
    cancelled(second);
}

void wiringFailuresThrow()
{
    throwsApplicationError([] { application::Lifecycle lifecycle{runtime::Executor{}}; },
        application::ErrorCode::missingDependency, "Lifecycle");
    Fixture fixture;
    auto entry = fixture.run();
    auto again = fixture.run();
    throwsApplicationError([&] { receive(again); }, application::ErrorCode::contractViolation, "already running");
    fixture.lifecycle->requestStop();
    receive(entry);
}
} // namespace

int main()
{
    try {
        startupAndShutdown();
        stopBeforeStartup();
        startupFailureAndRecovery();
        immediateShutdown();
        unexpectedFailureReleasesStartup();
        independentStartupSteps();
        laterStartupFailureReleasesRemainingConsumers();
        wiringFailuresThrow();
        std::print("Async application lifecycle checks passed.\n");
        return 0;
    } catch (const std::exception &error) {
        std::print(stderr, "Async application lifecycle check failed: {}\n", error.what());
        return 1;
    }
}
