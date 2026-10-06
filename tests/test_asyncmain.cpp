#include <asio/co_spawn.hpp>
#include <asio/strand.hpp>
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
    auto run()
    {
        return asio::co_spawn(asio::make_strand(runtime.executor()),
            application::async_main({tasks, lifecycle}), asio::use_future);
    }
    ~Fixture() { lifecycle->requestStop(); runtime.finish(); }
};
template <class T> auto receive(std::future<T> &future) -> T
{
    require(future.wait_for(std::chrono::seconds(5)) == future_status::ready);
    return future.get();
}
template <class T> void cancelled(std::future<T> &future)
{
    require(future.wait_for(std::chrono::seconds(5)) == future_status::ready);
    try { future.get(); require(false); }
    catch (const std::system_error &) {}
}

void startupAndShutdown()
{
    Fixture fixture;
    const auto records = storage::Strings{"seed", "Loaded by async_main", "0"};
    require(fixture.store->setStrings("tasks.items", records).has_value());
    auto startup = asio::co_spawn(fixture.runtime.executor(), fixture.lifecycle->startup(), asio::use_future);
    auto entry = fixture.run();
    const auto loaded = receive(startup);
    require(loaded.ready && loaded.changed && !loaded.error);
    require(loaded.tasks.size() == 1 && loaded.tasks.at(0).title == "Loaded by async_main");
    require(entry.wait_for(std::chrono::seconds(0)) == future_status::timeout);
    auto add = asio::co_spawn(fixture.tasks->executor(), fixture.tasks->addTask("While running"), asio::use_future);
    require(receive(add).tasks.size() == 2);
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
    auto startup = asio::co_spawn(fixture.runtime.executor(), fixture.lifecycle->startup(), asio::use_future);
    auto entry = fixture.run();
    receive(entry);
    cancelled(startup);
    require(!std::filesystem::exists(fixture.directory.path / "mmkv"));
}

void startupFailureAndRecovery()
{
    Fixture fixture;
    const auto path = fixture.directory.path / "mmkv";
    { auto blocker = std::ofstream{path}; blocker << "blocked"; }
    auto startup = asio::co_spawn(fixture.runtime.executor(), fixture.lifecycle->startup(), asio::use_future);
    auto entry = fixture.run();
    const auto failed = receive(startup);
    require(!failed.ready && failed.error && failed.error->code == business::ErrorCode::storage);
    require(entry.wait_for(std::chrono::seconds(0)) == future_status::timeout);
    std::filesystem::remove(path);
    auto retry = asio::co_spawn(fixture.tasks->executor(), fixture.tasks->reload(), asio::use_future);
    require(receive(retry).ready);
    fixture.lifecycle->requestStop();
    receive(entry);
}

void immediateShutdown()
{
    for (auto index = 0; index < 30; ++index) {
        Fixture fixture;
        auto startup = asio::co_spawn(fixture.runtime.executor(), fixture.lifecycle->startup(), asio::use_future);
        auto entry = fixture.run();
        fixture.lifecycle->requestStop();
        receive(entry);
        require(startup.wait_for(std::chrono::seconds(5)) == future_status::ready);
        try { static_cast<void>(startup.get()); }
        catch (const std::system_error &) {}
        fixture.runtime.finish();
    }
}

void unexpectedFailureReleasesStartup()
{
    Fixture fixture;
    fixture.tasks.reset();
    auto startup = asio::co_spawn(fixture.runtime.executor(), fixture.lifecycle->startup(), asio::use_future);
    auto entry = fixture.run();
    require(entry.wait_for(std::chrono::seconds(5)) == future_status::ready);
    try { entry.get(); require(false); }
    catch (const std::invalid_argument &) {}
    cancelled(startup);
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
        std::print("Async application lifecycle checks passed.\n");
        return 0;
    } catch (const std::exception &error) {
        std::print(stderr, "Async application lifecycle check failed: {}\n", error.what());
        return 1;
    }
}
