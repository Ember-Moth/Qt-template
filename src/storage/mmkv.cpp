module Template.Storage.Mmkv;
import :sdk;
import std;

using std::string;
using std::string_view;
using std::vector;
using std::span;
using std::shared_ptr;
using std::mutex;
using std::once_flag;
using std::map;
using std::scoped_lock;
using std::error_code;
using std::ifstream;
using std::optional;
using std::size_t;
using std::uintmax_t;
using std::uint32_t;
using std::uint8_t;
using std::streamsize;
using std::int64_t;
using std::uint64_t;
using std::byte;
using std::ios;
using std::nullopt;
using std::format;
using errors::Ok;
using errors::Err;
using std::make_shared;
using std::make_unique;
using std::call_once;
using std::memcpy;
using std::bad_alloc;
using std::exception;
using std::pair;
using std::ranges::all_of;
namespace fs = std::filesystem;

namespace storage {
namespace {
using StoreKey = std::pair<fs::path, string>;

struct FileStamp
{
    uintmax_t size;
    fs::file_time_type time;
    bool operator==(const FileStamp &) const = default;
};
struct Fingerprint
{
    FileStamp data;
    FileStamp metadata;
    bool operator==(const Fingerprint &) const = default;
};
// Shared by aliases: serializes access and remembers the files as last checked or written.
struct Operations
{
    mutex gate;
    optional<Fingerprint> verified;
};

struct StoreEntry
{
    MMKV *store;
    size_t references;
    bool readOnly;
    shared_ptr<Operations> operations;
};
struct StoreRegistry
{
    mutex gate;
    once_flag initialization;
    bool initialized = false;
    map<StoreKey, StoreEntry> stores;
    ~StoreRegistry() { if (initialized) MMKV::onExit(); }
};
auto registry() -> StoreRegistry &
{
    static StoreRegistry instance;
    return instance;
}

// Failures carry the reason and file name; the store adds the operation and location.
auto validateStore(const fs::path &directory, const string &identifier) -> Result<void>
{
    const auto data = directory / identifier;
    const auto metadata = directory / (identifier + ".crc");
    const auto dataName = identifier, metadataName = identifier + ".crc";
    auto error = error_code{};
    const auto dataExists = fs::exists(data, error);
    if (error) return Err(ErrorCode::io, "cannot inspect data file '{}': {}", dataName, error.message());
    const auto metadataExists = fs::exists(metadata, error);
    if (error) return Err(ErrorCode::io, "cannot inspect metadata file '{}': {}", metadataName, error.message());
    if (!dataExists && !metadataExists) return Ok();
    if (!dataExists || !metadataExists)
        return Err(ErrorCode::invalidFormat, "{} file '{}' is missing while '{}' exists",
            dataExists ? "metadata" : "data", dataExists ? metadataName : dataName, dataExists ? dataName : metadataName);
    if (!fs::is_regular_file(data, error) || error
        || !fs::is_regular_file(metadata, error) || error)
        return Err(ErrorCode::invalidFormat, "'{}' and '{}' must be regular files", dataName, metadataName);
    if (fs::file_size(data, error) < sizeof(uint32_t) || error
        || fs::file_size(metadata, error) < sizeof(MMKVMetaInfo) || error)
        return Err(ErrorCode::invalidFormat, "'{}' or '{}' is truncated", dataName, metadataName);
    // MMKV 2.x keeps actualSize in metadata; its legacy static validator reads the old header.
    auto metadataFile = ifstream{metadata, ios::binary};
    auto info = MMKVMetaInfo{};
    metadataFile.read(reinterpret_cast<char *>(&info), sizeof(info));
    auto dataFile = ifstream{data, ios::binary};
    auto legacySize = uint32_t{};
    dataFile.read(reinterpret_cast<char *>(&legacySize), sizeof(legacySize));
    if (!metadataFile || !dataFile)
        return Err(ErrorCode::io, "cannot read the headers of '{}' and '{}'", dataName, metadataName);
    if (info.m_version > MMKVVersionFlag)
        return Err(ErrorCode::invalidFormat, "metadata version {} in '{}' is newer than supported version {}",
            static_cast<uint32_t>(info.m_version), metadataName, static_cast<uint32_t>(MMKVVersionFlag));
    const auto size = info.m_version >= MMKVVersionActualSize ? info.m_actualSize : legacySize;
    const auto available = fs::file_size(data, error) - sizeof(legacySize);
    if (size > available || error)
        return Err(ErrorCode::invalidFormat, "payload length {} exceeds the {} bytes in '{}'", size, available, dataName);
    auto bytes = string(size, '\0');
    dataFile.read(bytes.data(), static_cast<streamsize>(bytes.size()));
    if (!dataFile) return Err(ErrorCode::io, "cannot read the {}-byte payload of '{}'", size, dataName);
    // Reject corruption before opening: MMKV's default recovery can discard data.
    if (checksum(0, reinterpret_cast<const uint8_t *>(bytes.data()), size) != info.m_crcDigest)
        return Err(ErrorCode::invalidFormat, "checksum of '{}' does not match '{}'; the data is corrupted", dataName, metadataName);
    return Ok();
}
auto display(const fs::path &path) -> string
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

auto stamp(const fs::path &file) -> optional<FileStamp>
{
    auto error = error_code{};
    const auto size = fs::file_size(file, error);
    if (error) return nullopt;
    const auto time = fs::last_write_time(file, error);
    if (error) return nullopt;
    return FileStamp{size, time};
}
auto fingerprint(const fs::path &directory, const string &identifier) -> optional<Fingerprint>
{
    const auto data = stamp(directory / identifier);
    const auto metadata = stamp(directory / (identifier + ".crc"));
    if (!data || !metadata) return nullopt;
    return Fingerprint{*data, *metadata};
}

} // namespace

struct MmkvStore::Impl
{
    fs::path directory;
    string identifier;
    bool readOnly;
    mutable mutex gate;
    mutable MMKV *store = nullptr;
    mutable fs::path canonical;
    mutable shared_ptr<Operations> operations;

    Impl(fs::path path, string id, bool mode)
        : directory(std::move(path)), identifier(std::move(id)), readOnly(mode) {}
    ~Impl()
    {
        if (!store) return;
        auto &pool = registry();
        const auto lock = scoped_lock(pool.gate);
        const auto found = pool.stores.find(StoreKey{canonical, identifier});
        if (--found->second.references == 0) {
            // Open, acquire, release and terminal close share one lock: no alias can race close().
            found->second.store->close();
            pool.stores.erase(found);
        }
    }

    // The operation and the store it ran on, as the context of its failures.
    auto where(string_view action) const -> string
    {
        return format("{} MMKV store '{}' in '{}'", action, identifier, display(directory));
    }
    // MMKV reports some I/O failures by throwing; they are recoverable, unlike running out of memory.
    template <class T, class Call>
    static auto guarded(Call call) -> Result<T>
    {
        try {
            return call();
        } catch (const bad_alloc &) {
            throw;
        } catch (const exception &error) {
            return Err(ErrorCode::io, "MMKV raised: {}", error.what());
        }
    }

    auto open() const -> Result<void>
    {
        if (store) co_return Ok();
        // Portable instance IDs map directly to data/metadata file names.
        if (identifier.empty() || identifier == "." || identifier == ".."
            || !all_of(identifier, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            }))
            co_return Err(ErrorCode::invalidIdentifier,
                "the instance ID must be non-empty and use only ASCII letters, digits, '_', '-' and '.'");
        auto error = error_code{};
        if (!readOnly) fs::create_directories(directory, error);
        if (error) co_return Err(ErrorCode::io, "cannot create the directory: {}", error.message());
        canonical = fs::canonical(directory, error);
        if (error) co_return Err(ErrorCode::io, "cannot resolve the directory: {}", error.message());
        const auto native = canonical.native();
        auto &pool = registry();
        const auto lock = scoped_lock(pool.gate);
        co_await guarded<void>([&] -> Result<void> {
            call_once(pool.initialization, [&] {
                MMKV::initializeMMKV(native, MMKVLogError);
                pool.initialized = true;
            });
            return Ok();
        });
        if (const auto found = pool.stores.find(StoreKey{canonical, identifier}); found != pool.stores.end()) {
            if (found->second.readOnly != readOnly)
                co_return Err(ErrorCode::io, "the store is already open {}",
                    found->second.readOnly ? "read-only" : "for writing");
            ++found->second.references;
            store = found->second.store;
            operations = found->second.operations;
            co_return Ok();
        }
        if (readOnly && !fs::exists(canonical / identifier, error))
            co_return Err(ErrorCode::io, "a read-only store must already exist");
        if (error) co_return Err(ErrorCode::io, "cannot inspect the data file: {}", error.message());
        co_await validateStore(canonical, identifier);
        auto config = MMKVConfig{};
        config.mode = accessMode(readOnly);
        config.rootPath = &native;
        auto *opened = co_await guarded<MMKV *>([&] -> Result<MMKV *> { return MMKV::mmkvWithID(identifier, config); });
        if (!opened) co_return Err(ErrorCode::io, "MMKV could not open the instance");
        store = opened;
        operations = make_shared<Operations>();
        pool.stores.emplace(StoreKey{canonical, identifier}, StoreEntry{store, 1, readOnly, operations});
        co_return Ok();
    }

    auto validate() const -> Result<void>
    {
        // Skip the full CRC pass while the files match the last check or locked operation.
        if (operations->verified && operations->verified == fingerprint(canonical, identifier)) return Ok();
        auto error = error_code{};
        if (!fs::is_directory(canonical, error) || error)
            return Err(ErrorCode::io, "the directory is no longer available");
        return validateStore(canonical, identifier);
    }

    template <class T, class Operation>
    auto access(string_view action, Operation operation, bool writing = false) const -> Result<T>
    {
        const auto lock = scoped_lock(gate);
        const auto context = where(action);
        if (writing && readOnly) co_return Err(ErrorCode::io, "{}: the store is open read-only", context);
        co_await open().context(context);
        const auto serialized = scoped_lock(operations->gate);
        co_await validate().context(context);
        auto result = guarded<T>([&] { return operation(*store); });
        // MMKV's own changes are trusted; outside edits show a new size or modification time.
        operations->verified = fingerprint(canonical, identifier);
        co_return std::move(result).context(context);
    }
    auto keyed(string_view key, string_view action) const -> Result<void>
    {
        if (key.empty()) return Err(ErrorCode::invalidKey, "{}: the key is empty", where(action));
        return Ok();
    }
    template <class T, class Getter>
    auto read(string_view key, Getter getter) const -> Result<optional<T>>
    {
        const auto action = format("read '{}' from", key);
        co_await keyed(key, action);
        co_return access<optional<T>>(action, [&](MMKV &handle) -> Result<optional<T>> {
            if (!handle.containsKey(key)) return nullopt;
            T value{};
            if (!getter(handle, key, value))
                return Err(ErrorCode::invalidFormat, "the stored value has a different type");
            return optional<T>{std::move(value)};
        });
    }
    template <class Setter>
    auto write(string_view key, Setter setter, string_view verb = "write", string_view preposition = "to") const
        -> Result<void>
    {
        const auto action = format("{} '{}' {}", verb, key, preposition);
        co_await keyed(key, action);
        co_return access<void>(action, [&](MMKV &handle) -> Result<void> {
            if (!setter(handle, key)) return Err(ErrorCode::io, "MMKV rejected the change");
            // set/remove report success; sync() itself provides no durability result.
            handle.sync(MMKV_SYNC);
            return Ok();
        }, true);
    }
};

MmkvStore::MmkvStore(fs::path directory, string identifier, bool readOnly)
    : m_impl(make_unique<Impl>(std::move(directory), std::move(identifier), readOnly)) {}
MmkvStore::~MmkvStore() = default;

auto MmkvStore::getBool(string_view key) const -> Result<optional<bool>>
{
    return m_impl->read<bool>(key, [](MMKV &handle, auto key, bool &value) {
        auto found = false;
        value = handle.getBool(key, false, &found);
        return found;
    });
}
auto MmkvStore::setBool(string_view key, bool value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getInt64(string_view key) const -> Result<optional<int64_t>>
{
    return m_impl->read<int64_t>(key, [](MMKV &handle, auto key, int64_t &value) {
        auto found = false;
        value = handle.getInt64(key, 0, &found);
        return found;
    });
}
auto MmkvStore::setInt64(string_view key, int64_t value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getUInt64(string_view key) const -> Result<optional<uint64_t>>
{
    return m_impl->read<uint64_t>(key, [](MMKV &handle, auto key, uint64_t &value) {
        auto found = false;
        value = handle.getUInt64(key, 0, &found);
        return found;
    });
}
auto MmkvStore::setUInt64(string_view key, uint64_t value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getDouble(string_view key) const -> Result<optional<double>>
{
    return m_impl->read<double>(key, [](MMKV &handle, auto key, double &value) {
        auto found = false;
        value = handle.getDouble(key, 0.0, &found);
        return found;
    });
}
auto MmkvStore::setDouble(string_view key, double value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getString(string_view key) const -> Result<optional<string>>
{
    return m_impl->read<string>(key, [](MMKV &handle, auto key, string &value) { return handle.getString(key, value); });
}
auto MmkvStore::setString(string_view key, string_view value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getStrings(string_view key) const -> Result<optional<Strings>>
{
    return m_impl->read<Strings>(key, [](MMKV &handle, auto key, Strings &value) { return handle.getVector(key, value); });
}
auto MmkvStore::setStrings(string_view key, span<const string> value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) {
        return handle.set(Strings(value.begin(), value.end()), key);
    });
}
auto MmkvStore::getBytes(string_view key) const -> Result<optional<Bytes>>
{
    return m_impl->read<Bytes>(key, [](MMKV &handle, auto key, Bytes &value) {
        auto buffer = MMBuffer{};
        if (!handle.getBytes(key, buffer)) return false;
        value.resize(buffer.length());
        if (!value.empty()) memcpy(value.data(), buffer.getPtr(), buffer.length());
        return true;
    });
}
auto MmkvStore::setBytes(string_view key, span<const byte> value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) {
        auto buffer = MMBuffer(value.size());
        if (!value.empty()) memcpy(buffer.getPtr(), value.data(), value.size());
        return handle.set(buffer, key);
    });
}
auto MmkvStore::contains(string_view key) const -> Result<bool>
{
    const auto action = format("look up '{}' in", key);
    co_await m_impl->keyed(key, action);
    co_return m_impl->access<bool>(action, [key](MMKV &handle) -> Result<bool> { return handle.containsKey(key); });
}
auto MmkvStore::keys() const -> Result<Strings>
{
    return m_impl->access<Strings>("list the keys of", [](MMKV &handle) -> Result<Strings> { return handle.allKeys(); });
}
auto MmkvStore::remove(string_view key) const -> Result<void>
{
    return m_impl->write(key, [](MMKV &handle, auto key) {
        return !handle.containsKey(key) || handle.removeValueForKey(key);
    }, "remove", "from");
}
auto MmkvStore::clear() const -> Result<void>
{
    return m_impl->access<void>("clear", [](MMKV &handle) -> Result<void> {
        handle.clearAll();
        handle.sync(MMKV_SYNC);
        if (handle.count() != 0) return Err(ErrorCode::io, "keys remain after clearing");
        return Ok();
    }, true);
}
} // namespace storage
