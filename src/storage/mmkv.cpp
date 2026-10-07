module Template.Storage.Mmkv;
import :sdk;
import std;

using std::string;
using std::string_view;
using std::vector;
using std::span;
using std::expected;
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

namespace storage {
namespace {
auto storageError(string detail) -> Error { return {ErrorCode::io, std::move(detail)}; }
auto formatError(string detail) -> Error { return {ErrorCode::invalidFormat, std::move(detail)}; }
using StoreKey = std::pair<std::filesystem::path, string>;

struct FileStamp
{
    uintmax_t size;
    std::filesystem::file_time_type time;
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
auto validateStore(const std::filesystem::path &directory, const string &identifier) -> Result<void>
{
    const auto data = directory / identifier;
    const auto metadata = directory / (identifier + ".crc");
    const auto dataName = identifier, metadataName = identifier + ".crc";
    auto error = error_code{};
    const auto dataExists = std::filesystem::exists(data, error);
    if (error) return std::unexpected(storageError(std::format("cannot inspect data file '{}': {}", dataName, error.message())));
    const auto metadataExists = std::filesystem::exists(metadata, error);
    if (error) return std::unexpected(storageError(std::format("cannot inspect metadata file '{}': {}", metadataName, error.message())));
    if (!dataExists && !metadataExists) return {};
    if (!dataExists || !metadataExists)
        return std::unexpected(formatError(std::format("{} file '{}' is missing while '{}' exists",
            dataExists ? "metadata" : "data", dataExists ? metadataName : dataName, dataExists ? dataName : metadataName)));
    if (!std::filesystem::is_regular_file(data, error) || error
        || !std::filesystem::is_regular_file(metadata, error) || error)
        return std::unexpected(formatError(std::format("'{}' and '{}' must be regular files", dataName, metadataName)));
    if (std::filesystem::file_size(data, error) < sizeof(uint32_t) || error
        || std::filesystem::file_size(metadata, error) < sizeof(MMKVMetaInfo) || error)
        return std::unexpected(formatError(std::format("'{}' or '{}' is truncated", dataName, metadataName)));
    // MMKV 2.x keeps actualSize in metadata; its legacy static validator reads the old header.
    auto metadataFile = ifstream{metadata, std::ios::binary};
    auto info = MMKVMetaInfo{};
    metadataFile.read(reinterpret_cast<char *>(&info), sizeof(info));
    auto dataFile = ifstream{data, std::ios::binary};
    auto legacySize = uint32_t{};
    dataFile.read(reinterpret_cast<char *>(&legacySize), sizeof(legacySize));
    if (!metadataFile || !dataFile)
        return std::unexpected(storageError(std::format("cannot read the headers of '{}' and '{}'", dataName, metadataName)));
    if (info.m_version > MMKVVersionFlag)
        return std::unexpected(formatError(std::format("metadata version {} in '{}' is newer than supported version {}",
            static_cast<uint32_t>(info.m_version), metadataName, static_cast<uint32_t>(MMKVVersionFlag))));
    const auto size = info.m_version >= MMKVVersionActualSize ? info.m_actualSize : legacySize;
    const auto available = std::filesystem::file_size(data, error) - sizeof(legacySize);
    if (size > available || error)
        return std::unexpected(formatError(std::format("payload length {} exceeds the {} bytes in '{}'", size, available, dataName)));
    auto bytes = string(size, '\0');
    dataFile.read(bytes.data(), static_cast<streamsize>(bytes.size()));
    if (!dataFile) return std::unexpected(storageError(std::format("cannot read the {}-byte payload of '{}'", size, dataName)));
    // Reject corruption before opening: MMKV's default recovery can discard data.
    if (checksum(0, reinterpret_cast<const uint8_t *>(bytes.data()), size) != info.m_crcDigest)
        return std::unexpected(formatError(std::format("checksum of '{}' does not match '{}'; the data is corrupted", dataName, metadataName)));
    return {};
}
auto display(const std::filesystem::path &path) -> string
{
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

auto stamp(const std::filesystem::path &file) -> optional<FileStamp>
{
    auto error = error_code{};
    const auto size = std::filesystem::file_size(file, error);
    if (error) return std::nullopt;
    const auto time = std::filesystem::last_write_time(file, error);
    if (error) return std::nullopt;
    return FileStamp{size, time};
}
auto fingerprint(const std::filesystem::path &directory, const string &identifier) -> optional<Fingerprint>
{
    const auto data = stamp(directory / identifier);
    const auto metadata = stamp(directory / (identifier + ".crc"));
    if (!data || !metadata) return std::nullopt;
    return Fingerprint{*data, *metadata};
}

} // namespace

struct MmkvStore::Impl
{
    std::filesystem::path directory;
    string identifier;
    bool readOnly;
    mutable mutex gate;
    mutable MMKV *store = nullptr;
    mutable std::filesystem::path canonical;
    mutable shared_ptr<Operations> operations;

    Impl(std::filesystem::path path, string id, bool mode)
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

    // Prefix a reason with the operation and the store it ran on.
    auto failure(Error error, string_view action) const -> Error
    {
        error.detail = std::format("{} MMKV store '{}' in '{}': {}", action, identifier, display(directory), error.detail);
        return error;
    }
    // MMKV reports some I/O failures by throwing; they are recoverable, unlike running out of memory.
    template <class T, class Call>
    static auto guarded(Call call) -> Result<T>
    {
        try {
            return call();
        } catch (const std::bad_alloc &) {
            throw;
        } catch (const std::exception &error) {
            return std::unexpected(storageError(std::format("MMKV raised: {}", error.what())));
        }
    }

    auto open() const -> Result<void>
    {
        if (store) return {};
        // Portable instance IDs map directly to data/metadata file names.
        if (identifier.empty() || identifier == "." || identifier == ".."
            || !std::ranges::all_of(identifier, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            }))
            return std::unexpected(Error{ErrorCode::invalidIdentifier,
                "the instance ID must be non-empty and use only ASCII letters, digits, '_', '-' and '.'"});
        auto error = error_code{};
        if (!readOnly) std::filesystem::create_directories(directory, error);
        if (error) return std::unexpected(storageError(std::format("cannot create the directory: {}", error.message())));
        canonical = std::filesystem::canonical(directory, error);
        if (error) return std::unexpected(storageError(std::format("cannot resolve the directory: {}", error.message())));
        const auto native = canonical.native();
        auto &pool = registry();
        const auto lock = scoped_lock(pool.gate);
        if (auto initialized = guarded<void>([&] -> Result<void> {
                std::call_once(pool.initialization, [&] {
                    MMKV::initializeMMKV(native, MMKVLogError);
                    pool.initialized = true;
                });
                return {};
            }); !initialized)
            return initialized;
        if (const auto found = pool.stores.find(StoreKey{canonical, identifier}); found != pool.stores.end()) {
            if (found->second.readOnly != readOnly)
                return std::unexpected(storageError(std::format("the store is already open {}",
                    found->second.readOnly ? "read-only" : "for writing")));
            ++found->second.references;
            store = found->second.store;
            operations = found->second.operations;
            return {};
        }
        if (readOnly && !std::filesystem::exists(canonical / identifier, error))
            return std::unexpected(storageError("a read-only store must already exist"));
        if (error) return std::unexpected(storageError(std::format("cannot inspect the data file: {}", error.message())));
        if (auto valid = validateStore(canonical, identifier); !valid) return valid;
        auto config = MMKVConfig{};
        config.mode = accessMode(readOnly);
        config.rootPath = &native;
        auto opened = guarded<MMKV *>([&] -> Result<MMKV *> { return MMKV::mmkvWithID(identifier, config); });
        if (!opened) return std::unexpected(opened.error());
        if (!*opened) return std::unexpected(storageError("MMKV could not open the instance"));
        store = *opened;
        operations = std::make_shared<Operations>();
        pool.stores.emplace(StoreKey{canonical, identifier}, StoreEntry{store, 1, readOnly, operations});
        return {};
    }

    auto validate() const -> Result<void>
    {
        // Skip the full CRC pass while the files match the last check or locked operation.
        if (operations->verified && operations->verified == fingerprint(canonical, identifier)) return {};
        auto error = error_code{};
        if (!std::filesystem::is_directory(canonical, error) || error)
            return std::unexpected(storageError("the directory is no longer available"));
        return validateStore(canonical, identifier);
    }

    template <class T, class Operation>
    auto access(string_view action, Operation operation, bool writing = false) const -> Result<T>
    {
        const auto lock = scoped_lock(gate);
        if (writing && readOnly)
            return std::unexpected(failure(storageError("the store is open read-only"), action));
        if (auto opened = open(); !opened) return std::unexpected(failure(opened.error(), action));
        const auto serialized = scoped_lock(operations->gate);
        if (auto valid = validate(); !valid) return std::unexpected(failure(valid.error(), action));
        Result<T> result = guarded<T>([&] { return operation(*store); });
        // MMKV's own changes are trusted; outside edits show a new size or modification time.
        operations->verified = fingerprint(canonical, identifier);
        if (!result) return std::unexpected(failure(std::move(result).error(), action));
        return result;
    }
    auto keyed(string_view key, string_view action) const -> Result<void>
    {
        if (key.empty()) return std::unexpected(failure(Error{ErrorCode::invalidKey, "the key is empty"}, action));
        return {};
    }
    template <class T, class Getter>
    auto read(std::string_view key, Getter getter) const -> Result<std::optional<T>>
    {
        const auto action = std::format("read '{}' from", key);
        if (auto valid = keyed(key, action); !valid) return std::unexpected(valid.error());
        return access<std::optional<T>>(action, [&](MMKV &handle) -> Result<std::optional<T>> {
            if (!handle.containsKey(key)) return std::nullopt;
            T value{};
            if (!getter(handle, key, value))
                return std::unexpected(formatError("the stored value has a different type"));
            return std::optional<T>{std::move(value)};
        });
    }
    template <class Setter>
    auto write(std::string_view key, Setter setter, string_view verb = "write", string_view preposition = "to") const
        -> Result<void>
    {
        const auto action = std::format("{} '{}' {}", verb, key, preposition);
        if (auto valid = keyed(key, action); !valid) return valid;
        return access<void>(action, [&](MMKV &handle) -> Result<void> {
            if (!setter(handle, key)) return std::unexpected(storageError("MMKV rejected the change"));
            // set/remove report success; sync() itself provides no durability result.
            handle.sync(MMKV_SYNC);
            return {};
        }, true);
    }
};

MmkvStore::MmkvStore(std::filesystem::path directory, string identifier, bool readOnly)
    : m_impl(std::make_unique<Impl>(std::move(directory), std::move(identifier), readOnly)) {}
MmkvStore::~MmkvStore() = default;

auto MmkvStore::getBool(std::string_view key) const -> Result<std::optional<bool>>
{
    return m_impl->read<bool>(key, [](MMKV &handle, auto key, bool &value) {
        auto found = false;
        value = handle.getBool(key, false, &found);
        return found;
    });
}
auto MmkvStore::setBool(std::string_view key, bool value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getInt64(std::string_view key) const -> Result<std::optional<std::int64_t>>
{
    return m_impl->read<std::int64_t>(key, [](MMKV &handle, auto key, std::int64_t &value) {
        auto found = false;
        value = handle.getInt64(key, 0, &found);
        return found;
    });
}
auto MmkvStore::setInt64(std::string_view key, std::int64_t value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getUInt64(std::string_view key) const -> Result<std::optional<std::uint64_t>>
{
    return m_impl->read<std::uint64_t>(key, [](MMKV &handle, auto key, std::uint64_t &value) {
        auto found = false;
        value = handle.getUInt64(key, 0, &found);
        return found;
    });
}
auto MmkvStore::setUInt64(std::string_view key, std::uint64_t value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getDouble(std::string_view key) const -> Result<std::optional<double>>
{
    return m_impl->read<double>(key, [](MMKV &handle, auto key, double &value) {
        auto found = false;
        value = handle.getDouble(key, 0.0, &found);
        return found;
    });
}
auto MmkvStore::setDouble(std::string_view key, double value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getString(std::string_view key) const -> Result<std::optional<string>>
{
    return m_impl->read<string>(key, [](MMKV &handle, auto key, string &value) { return handle.getString(key, value); });
}
auto MmkvStore::setString(std::string_view key, std::string_view value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) { return handle.set(value, key); });
}
auto MmkvStore::getStrings(std::string_view key) const -> Result<std::optional<Strings>>
{
    return m_impl->read<Strings>(key, [](MMKV &handle, auto key, Strings &value) { return handle.getVector(key, value); });
}
auto MmkvStore::setStrings(std::string_view key, span<const string> value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) {
        return handle.set(Strings(value.begin(), value.end()), key);
    });
}
auto MmkvStore::getBytes(std::string_view key) const -> Result<std::optional<Bytes>>
{
    return m_impl->read<Bytes>(key, [](MMKV &handle, auto key, Bytes &value) {
        auto buffer = MMBuffer{};
        if (!handle.getBytes(key, buffer)) return false;
        value.resize(buffer.length());
        if (!value.empty()) std::memcpy(value.data(), buffer.getPtr(), buffer.length());
        return true;
    });
}
auto MmkvStore::setBytes(std::string_view key, span<const std::byte> value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) {
        auto buffer = MMBuffer(value.size());
        if (!value.empty()) std::memcpy(buffer.getPtr(), value.data(), value.size());
        return handle.set(buffer, key);
    });
}
auto MmkvStore::contains(std::string_view key) const -> Result<bool>
{
    const auto action = std::format("look up '{}' in", key);
    if (auto valid = m_impl->keyed(key, action); !valid) return std::unexpected(valid.error());
    return m_impl->access<bool>(action, [key](MMKV &handle) -> Result<bool> { return handle.containsKey(key); });
}
auto MmkvStore::keys() const -> Result<Strings>
{
    return m_impl->access<Strings>("list the keys of", [](MMKV &handle) -> Result<Strings> { return handle.allKeys(); });
}
auto MmkvStore::remove(std::string_view key) const -> Result<void>
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
        if (handle.count() != 0) return std::unexpected(storageError("keys remain after clearing"));
        return {};
    }, true);
}
} // namespace storage
