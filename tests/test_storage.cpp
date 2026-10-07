import std;
import Template.Storage.Mmkv;

using std::string;
using std::string_view;
using std::error_code;
using std::ifstream;
using std::ofstream;
using std::fstream;
using std::jthread;
using std::atomic;
using std::source_location;
using std::runtime_error;

namespace {
void require(bool condition, source_location where = source_location::current())
{
    if (!condition) throw runtime_error(std::format("{}:{} check failed", where.file_name(), where.line()));
}
struct TemporaryDirectory
{
    std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("qt-mmkv-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TemporaryDirectory() { std::filesystem::create_directories(path); }
    ~TemporaryDirectory() { error_code ignored; std::filesystem::remove_all(path, ignored); }
};
auto readFile(const std::filesystem::path &path)
{
    auto file = ifstream(path, std::ios::binary);
    require(file.is_open());
    return string{std::istreambuf_iterator<char>{file}, {}};
}
void writeFile(const std::filesystem::path &path, string_view content)
{
    auto file = ofstream(path, std::ios::binary | std::ios::trunc);
    require(file.is_open());
    file << content;
    require(file.good());
}
void patchFile(const std::filesystem::path &path, std::streamoff offset, char value)
{
    auto file = fstream(path, std::ios::binary | std::ios::in | std::ios::out);
    require(file.is_open());
    file.seekp(offset).put(value).flush();
    require(file.good());
}
void typedValues()
{
    TemporaryDirectory directory;
    const auto root = directory.path / std::filesystem::path{u8"数据"};
    const auto bytes = storage::Bytes{std::byte{0}, std::byte{255}, std::byte{42}};
    const auto strings = storage::Strings{"你好", "", "shared storage"};
    {
        storage::MmkvStore store(root);
        require(!store.getString("missing")->has_value());
        require(store.setBool("enabled", false).has_value());
        require(store.setInt64("signed", std::numeric_limits<std::int64_t>::min()).has_value());
        require(store.setUInt64("unsigned", std::numeric_limits<std::uint64_t>::max()).has_value());
        require(store.setDouble("ratio", -1.25).has_value());
        require(store.setString("name", "学习 QML").has_value());
        require(store.setString("empty", "").has_value());
        require(store.setBytes("bytes", bytes).has_value());
        require(store.setBytes("emptyBytes", {}).has_value());
        require(store.setStrings("list", strings).has_value());
        require(store.setStrings("emptyList", {}).has_value());
        require(store.getBool("enabled")->has_value() && !store.getBool("enabled")->value());
        require(store.getString("empty")->has_value() && store.getString("empty")->value().empty());
        require(store.getBytes("emptyBytes")->has_value() && store.getBytes("emptyBytes")->value().empty());
        require(store.getStrings("emptyList")->has_value() && store.getStrings("emptyList")->value().empty());
        require(!store.setString("", "invalid") && !store.getString(""));
        require(!store.remove("") && !store.contains(""));
    }
    {
        storage::MmkvStore reopened(root);
        require(reopened.getInt64("signed")->value() == std::numeric_limits<std::int64_t>::min());
        require(reopened.getUInt64("unsigned")->value() == std::numeric_limits<std::uint64_t>::max());
        require(reopened.getDouble("ratio")->value() == -1.25);
        require(reopened.getString("name")->value() == "学习 QML");
        require(reopened.getBytes("bytes")->value() == bytes);
        require(reopened.getStrings("list")->value() == strings);
        require(*reopened.contains("name") && reopened.keys()->size() == 10);
        require(reopened.remove("name").has_value() && !*reopened.contains("name"));
        require(reopened.remove("name").has_value());
    }
    {
        storage::MmkvStore readOnly(root, "app", true);
        require(readOnly.getStrings("list")->value() == strings);
        require(!readOnly.setBool("enabled", true) && !readOnly.remove("list") && !readOnly.clear());
        require(!readOnly.getBool("enabled")->value());
    }
    {
        storage::MmkvStore store(root);
        require(store.clear().has_value() && store.keys()->empty());
        require(!store.getBytes("bytes")->has_value());
    }
    require(!storage::MmkvStore(root, "../outside").keys());
    require(!storage::MmkvStore(root, "").keys());
}
void instances()
{
    TemporaryDirectory directory;
    auto original = std::make_unique<storage::MmkvStore>(directory.path, "settings");
    require(original->setString("theme", "dark").has_value());
    storage::MmkvStore alias(directory.path, "settings"), account(directory.path, "account");
    require(alias.getString("theme")->value() == "dark");
    require(!account.getString("theme")->has_value());
    require(account.setString("theme", "light").has_value());
    original.reset();
    require(alias.setString("theme", "system").has_value());
    require(alias.getString("theme")->value() == "system");
    require(account.getString("theme")->value() == "light");
    require(!storage::MmkvStore(directory.path, "settings", true).keys());
    auto failures = atomic{0};
    const auto exercise = [&](auto &store, string_view key) {
        for (auto index = 0; index < 20; ++index) {
            if (!store.setInt64(key, index)) ++failures;
            const auto value = store.getInt64(key);
            if (!value || !value->has_value()) ++failures;
        }
    };
    storage::MmkvStore other(directory.path, "settings");
    auto first = jthread([&] { exercise(alias, "one"); });
    auto second = jthread([&] { exercise(other, "two"); });
    first.join(); second.join();
    require(failures == 0);
}
void corruptedStore()
{
    TemporaryDirectory directory;
    {
        storage::MmkvStore store(directory.path, "settings");
        require(store.setString("name", "Keep original data").has_value());
    }
    const auto dataPath = directory.path / "settings";
    const auto metadataPath = directory.path / "settings.crc";
    const auto original = readFile(dataPath), metadata = readFile(metadataPath);
    auto corrupted = original;
    require(corrupted.size() > 12);
    corrupted[12] ^= 0x7f;
    {
        // Outside edits to an open store trigger the full check again.
        storage::MmkvStore store(directory.path, "settings");
        require(store.getString("name")->value() == "Keep original data");
        const auto time = std::filesystem::last_write_time(dataPath);
        // Coarse file clocks may not tick between writes; move the timestamp explicitly.
        patchFile(dataPath, 12, corrupted[12]);
        std::filesystem::last_write_time(dataPath, time + std::chrono::seconds(1));
        require(!store.getString("name") && !store.setString("name", "Overwrite"));
        patchFile(dataPath, 12, original[12]);
        std::filesystem::last_write_time(dataPath, time + std::chrono::seconds(2));
        require(store.getString("name")->value() == "Keep original data");
    }
    writeFile(dataPath, corrupted);
    {
        storage::MmkvStore store(directory.path, "settings");
        require(!store.getString("name") && !store.setString("name", "Overwrite"));
        require(readFile(dataPath) == corrupted && readFile(metadataPath) == metadata);
    }
    writeFile(dataPath, original);
    writeFile(metadataPath, "bad");
    require(!storage::MmkvStore(directory.path, "settings").keys());
    require(readFile(metadataPath) == "bad" && readFile(dataPath) == original);
    writeFile(metadataPath, metadata);
    require(storage::MmkvStore(directory.path, "settings").getString("name")->value() == "Keep original data");
}
} // namespace

int main()
{
    try {
        typedValues(); instances(); corruptedStore();
        std::println("PASS generic MMKV persistence, instance isolation, aliases and corruption handling");
    } catch (const std::exception &error) {
        std::println(std::cerr, "FAIL storage: {}", error.what());
        return 1;
    }
}
