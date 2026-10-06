#include <asio/co_spawn.hpp>
#include <asio/post.hpp>
#include <asio/use_future.hpp>

import std;
import Template.Tasks;
import Template.Storage.Mmkv;
import Template.Runtime.Asio;

using std::string;
using std::string_view;
using std::vector;
using std::span;
using std::optional;
using std::error_code;
using std::ifstream;
using std::jthread;
using std::exception;
using std::source_location;
using std::runtime_error;
using std::ofstream;
using std::atomic;
using std::future;
using std::array;
using std::pair;
using std::promise;

namespace {
void require(bool condition, source_location where = source_location::current())
{
    if (!condition)
        throw runtime_error(std::format("{}:{} check failed", where.file_name(), where.line()));
}
struct TemporaryDirectory {
    std::filesystem::path path = std::filesystem::temp_directory_path() / ("qt-template-" + business::createTaskId());
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { error_code ignored; std::filesystem::remove_all(path, ignored); }
};
auto readFile(const std::filesystem::path &path) -> string;
void writeFile(const std::filesystem::path &path, string_view content);
auto await(business::TaskService &service, asio::awaitable<business::Update> task)
{
    auto future = asio::co_spawn(service.executor(), std::move(task), asio::use_future);
    require(future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    return future.get();
}
void commands()
{
    TemporaryDirectory directory;
    auto store = std::make_shared<storage::MmkvStore>(directory.path / "mmkv");
    runtime::AsioRuntime asyncRuntime;
    business::TaskService service(asyncRuntime.executor(), store);
    auto loaded = await(service, service.reload());
    require(loaded.ready && !loaded.error);
    auto first = await(service, service.addTask("  First task  "));
    require(first.changed && first.tasks.at(0).title == "First task");
    auto second = await(service, service.addTask("Second task"));
    const auto firstId = first.tasks.at(0).id;
    const auto secondId = second.tasks.at(1).id;
    require(firstId != secondId);
    auto removed = await(service, service.removeTask(firstId));
    require(removed.tasks.size() == 1 && removed.tasks.at(0).id == secondId);
    auto completed = await(service, service.setTaskCompleted(secondId, true));
    require(completed.tasks.at(0).completed && completed.changed);
    const auto snapshot = readFile(directory.path / "mmkv" / "app");
    auto unchanged = await(service, service.setTaskCompleted(secondId, true));
    require(!unchanged.changed && !unchanged.error && readFile(directory.path / "mmkv" / "app") == snapshot);
    auto invalid = await(service, service.removeTask("missing"));
    require(invalid.error->code == business::ErrorCode::notFound);
    require(!business::normalizeTitle(" \t "));
    require(!business::normalizeTitle(string(121, 'x')));
    require(business::normalizeTitle(string(120, 'x')).has_value());
    require(*business::normalizeTitle("\xe3\x80\x80学习 QML\xc2\xa0") == "学习 QML");
    require(!business::normalizeTitle("\xc0\xaf"));
    require(!business::normalizeTitle("\xed\xa0\x80"));
    string emoji;
    for (int index = 0; index < 60; ++index) emoji += "\xf0\x9f\x98\x80";
    require(business::normalizeTitle(emoji).has_value());
    require(!business::normalizeTitle(emoji + "x"));
}
void failuresAndRecovery()
{
    TemporaryDirectory directory;
    const auto root = directory.path / "mmkv";
    runtime::AsioRuntime asyncRuntime;
    auto saved = business::Update{};
    {
        writeFile(root, "blocked");
        auto store = std::make_shared<storage::MmkvStore>(root);
        business::TaskService service(asyncRuntime.executor(), store);
        require(!await(service, service.reload()).ready);
        const auto blocked = await(service, service.addTask("Must not overwrite"));
        require(blocked.error->code == business::ErrorCode::notReady);
        require(readFile(root) == "blocked");
        std::filesystem::remove(root);
        require(await(service, service.reload()).ready);
        saved = await(service, service.addTask("Existing task"));
        require(saved.changed && !saved.error);
    }
    {
        auto store = std::make_shared<storage::MmkvStore>(root, "app", true);
        business::TaskService service(asyncRuntime.executor(), store);
        require(await(service, service.reload()).ready);
        const auto id = saved.tasks.at(0).id;
        const auto addition = await(service, service.addTask("Unsaved"));
        const auto completion = await(service, service.setTaskCompleted(id, true));
        const auto removal = await(service, service.removeTask(id));
        require(addition.error && completion.error && removal.error);
        require(!addition.changed && !completion.changed && !removal.changed);
        require(addition.tasks == saved.tasks && completion.tasks == saved.tasks && removal.tasks == saved.tasks);
        require(store->getStrings("tasks.items")->value().at(1) == saved.tasks.at(0).title);
    }
    {
        auto store = std::make_shared<storage::MmkvStore>(root);
        business::TaskService service(asyncRuntime.executor(), store);
        require(await(service, service.reload()).ready);
        require(await(service, service.setTaskCompleted(saved.tasks.at(0).id, true)).tasks.at(0).completed);
    }
}
auto readFile(const std::filesystem::path &path) -> string
{
    auto file = ifstream{path, std::ios::binary};
    require(file.is_open());
    return string{std::istreambuf_iterator<char>{file}, {}};
}
void writeFile(const std::filesystem::path &path, string_view content)
{
    auto file = ofstream{path, std::ios::binary | std::ios::trunc};
    require(file.is_open());
    file << content;
    require(file.good());
}
void setRawSnapshot(const std::filesystem::path &root, const vector<string> &records)
{
    storage::MmkvStore store(root);
    require(store.setStrings("tasks.items", records).has_value());
}
auto rawSnapshot(const std::filesystem::path &root)
{
    storage::MmkvStore store(root);
    const auto result = store.getStrings("tasks.items");
    require(result && result->has_value());
    return result->value();
}
void persistence()
{
    TemporaryDirectory directory;
    const auto root = directory.path / std::filesystem::path{u8"数据"} / "mmkv";
    auto tasks = business::Tasks{};
    runtime::AsioRuntime asyncRuntime;
    {
        auto store = std::make_shared<storage::MmkvStore>(root);
        business::TaskService service(asyncRuntime.executor(), store);
        require(await(service, service.reload()).ready);
        require(await(service, service.addTask("学习 QML")).changed);
        const auto added = await(service, service.addTask("Test persistence"));
        tasks = await(service, service.setTaskCompleted(added.tasks.back().id, true)).tasks;
    }
    {
        auto store = std::make_shared<storage::MmkvStore>(root);
        business::TaskService service(asyncRuntime.executor(), store);
        const auto loaded = await(service, service.reload());
        require(loaded.ready && loaded.tasks == tasks);
    }
    require(!business::validateTasks(business::Tasks{{"duplicate", "A"}, {"duplicate", "B"}}));
    require(!business::validateTasks(business::Tasks{{"overflow", string(120, ' ') + "A"}}));
}
void sharedStorageKeys()
{
    TemporaryDirectory directory;
    auto store = std::make_shared<storage::MmkvStore>(directory.path / "mmkv");
    require(store->setString("settings.theme", "dark").has_value());
    require(store->setBool("session.active", true).has_value());
    runtime::AsioRuntime asyncRuntime;
    business::TaskService service(asyncRuntime.executor(), store);
    require(await(service, service.reload()).ready);
    const auto added = await(service, service.addTask("Shared persistence"));
    require(added.changed);
    require(await(service, service.reload()).tasks == added.tasks);
    require(await(service, service.removeTask(added.tasks.front().id)).tasks.empty());
    require(store->getString("settings.theme")->value() == "dark");
    require(store->getBool("session.active")->value());
    require(store->getStrings("tasks.items")->value().empty());
}
void invalidSnapshots()
{
    const auto cases = vector<vector<string>>{
        {"id"}, {"id", "Title"}, {"id", "Title", "yes"},
        {"", "Title", "0"}, {"id", "", "0"},
        {"duplicate", "A", "0", "duplicate", "B", "1"},
        {"id", string(121, 'x'), "0"}
    };
    for (const auto &content : cases) {
        TemporaryDirectory directory;
        const auto root = directory.path / "mmkv";
        setRawSnapshot(root, content);
        {
            auto store = std::make_shared<storage::MmkvStore>(root);
            runtime::AsioRuntime asyncRuntime;
            business::TaskService service(asyncRuntime.executor(), store);
            require(!await(service, service.reload()).ready);
            require(await(service, service.addTask("Keep original data")).error.has_value());
        }
        require(rawSnapshot(root) == content);
    }
}
auto queuedWorkflow(vector<asio::awaitable<business::Update>> operations,
                    std::thread::id caller, bool &onWorker) -> asio::awaitable<business::Update>
{
    onWorker = std::this_thread::get_id() != caller;
    auto result = business::Update{};
    for (auto &operation : operations)
        result = co_await std::move(operation);
    co_return result;
}
void workerAndShutdown()
{
    TemporaryDirectory directory;
    auto store = std::make_shared<storage::MmkvStore>(directory.path / "mmkv");
    const auto caller = std::this_thread::get_id();
    auto onWorker = false;
    auto pending = future<business::Update>{};
    {
        runtime::AsioRuntime asyncRuntime;
        auto release = promise<void>{};
        const auto gate = release.get_future().share();
        asio::post(asyncRuntime.executor(), [gate] { gate.wait(); });
        {
            business::TaskService service(asyncRuntime.executor(), store);
            auto operations = vector<asio::awaitable<business::Update>>{};
            operations.push_back(service.reload());
            for (auto index = 0; index < 12; ++index)
                operations.push_back(service.addTask(std::format("Queued {}", index)));
            pending = asio::co_spawn(service.executor(), queuedWorkflow(std::move(operations), caller, onWorker), asio::use_future);
        }
        // The service is already gone when the worker can start its operations.
        release.set_value();
        // Runtime destruction drains the queued workflow before leaving scope.
    }
    const auto result = pending.get();
    require(onWorker && !result.error && result.tasks.size() == 12 && store->getStrings("tasks.items")->value().size() == 36);
}
auto workerThread() -> asio::awaitable<std::thread::id>
{
    co_return std::this_thread::get_id();
}
void sharedRuntime()
{
    runtime::AsioRuntime asyncRuntime;
    TemporaryDirectory directory;
    auto firstStore = std::make_shared<storage::MmkvStore>(directory.path / "first");
    auto secondStore = std::make_shared<storage::MmkvStore>(directory.path / "second");
    auto first = std::make_unique<business::TaskService>(asyncRuntime.executor(), firstStore);
    business::TaskService second(asyncRuntime.executor(), secondStore);
    require(await(*first, first->reload()).ready && await(second, second.reload()).ready);
    require(await(*first, first->addTask("First service")).tasks.size() == 1);

    const auto firstThread = asio::co_spawn(first->executor(), workerThread(), asio::use_future).get();
    const auto secondThread = asio::co_spawn(second.executor(), workerThread(), asio::use_future).get();
    const auto runtimeThread = asio::co_spawn(asyncRuntime.executor(), workerThread(), asio::use_future).get();
    require(firstThread == runtimeThread && secondThread == runtimeThread);
    require(runtimeThread != std::this_thread::get_id());
    first.reset();
    require(await(second, second.addTask("Second service")).tasks.size() == 1);
    require(firstStore->getStrings("tasks.items")->value().at(1) == "First service");
    require(secondStore->getStrings("tasks.items")->value().at(1) == "Second service");

    auto rejected = false;
    try {
        static_cast<void>(asio::co_spawn(asyncRuntime.executor(), second.reload(), asio::use_future).get());
    } catch (const std::logic_error &) {
        rejected = true;
    }
    require(rejected);
    require(await(second, second.addTask("Executor still works")).tasks.size() == 2);
    asyncRuntime.finish();
    asyncRuntime.finish();
}

}
int main(int argc, char *argv[])
{
    if (argc == 3) {
        storage::MmkvStore store(std::filesystem::path{argv[2]});
        if (string_view{argv[1]} == "--mmkv-write")
            return store.setStrings("tasks.items", vector<string>{"one", "学习 QML", "0", "two", "Test persistence", "1"}) ? 0 : 1;
        if (string_view{argv[1]} == "--mmkv-read") {
            const auto loaded = store.getStrings("tasks.items");
            return loaded && loaded->has_value() && loaded->value() == vector<string>{"one", "学习 QML", "0", "two", "Test persistence", "1"} ? 0 : 1;
        }
        return 1;
    }
    const array tests{
        pair{"commands", &commands}, pair{"failures/recovery", &failuresAndRecovery},
        pair{"persistence", &persistence}, pair{"invalid snapshots", &invalidSnapshots},
        pair{"shared storage keys", &sharedStorageKeys},
        pair{"runtime/shutdown", &workerAndShutdown},
        pair{"shared runtime", &sharedRuntime}
    };
    for (const auto &[name, test] : tests) {
        try { test(); std::println("PASS {}", name); }
        catch (const exception &error) { std::println(std::cerr, "FAIL {}: {}", name, error.what()); return 1; }
    }
}
