module;
// SDK headers pull in standard headers; MSVC STL supports them only before import std.
#include "MMKV/MMKV.h"
#include "MMKVMetaInfo.hpp"
#include "crc32/Checksum.h"

module Template.Storage.Mmkv;
import std;

using std::string;
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

auto validateStore(const std::filesystem::path &directory, const string &identifier) -> Result<void>
{
    const auto data = directory / identifier;
    const auto metadata = directory / (identifier + ".crc");
    auto error = error_code{};
    const auto dataExists = std::filesystem::exists(data, error);
    if (error) return std::unexpected(storageError(error.message()));
    const auto metadataExists = std::filesystem::exists(metadata, error);
    if (error) return std::unexpected(storageError(error.message()));
    if (!dataExists && !metadataExists) return {};
    if (!dataExists || !metadataExists)
        return std::unexpected(formatError("An MMKV data or metadata file is missing."));
    if (!std::filesystem::is_regular_file(data, error) || error
        || !std::filesystem::is_regular_file(metadata, error) || error)
        return std::unexpected(formatError("Invalid MMKV storage files."));
    if (std::filesystem::file_size(data, error) < sizeof(uint32_t) || error
        || std::filesystem::file_size(metadata, error) < sizeof(mmkv::MMKVMetaInfo) || error)
        return std::unexpected(formatError("Truncated MMKV storage files."));
    // MMKV 2.x keeps actualSize in metadata; its legacy static validator reads the old header.
    auto metadataFile = ifstream{metadata, std::ios::binary};
    auto info = mmkv::MMKVMetaInfo{};
    metadataFile.read(reinterpret_cast<char *>(&info), sizeof(info));
    auto dataFile = ifstream{data, std::ios::binary};
    auto legacySize = uint32_t{};
    dataFile.read(reinterpret_cast<char *>(&legacySize), sizeof(legacySize));
    if (!metadataFile || !dataFile)
        return std::unexpected(storageError("Cannot read MMKV storage files."));
    if (info.m_version > mmkv::MMKVVersionFlag)
        return std::unexpected(formatError("Unsupported MMKV metadata version."));
    const auto size = info.m_version >= mmkv::MMKVVersionActualSize ? info.m_actualSize : legacySize;
    if (size > std::filesystem::file_size(data, error) - sizeof(legacySize) || error)
        return std::unexpected(formatError("Invalid MMKV data length."));
    auto bytes = string(size, '\0');
    dataFile.read(bytes.data(), static_cast<streamsize>(bytes.size()));
    if (!dataFile) return std::unexpected(storageError("Cannot read MMKV payload."));
    // Reject corruption before opening: MMKV's default recovery can discard data.
    if (CRC32(0, reinterpret_cast<const uint8_t *>(bytes.data()), size) != info.m_crcDigest)
        return std::unexpected(formatError("MMKV checksum validation failed."));
    return {};
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

    auto open() const -> Result<void>
    {
        if (store) return {};
        // Portable instance IDs map directly to data/metadata file names.
        if (identifier.empty() || identifier == "." || identifier == ".."
            || !std::ranges::all_of(identifier, [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                    || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
            }))
            return std::unexpected(Error{ErrorCode::invalidIdentifier, "Use a non-empty portable MMKV instance ID."});
        auto error = error_code{};
        if (!readOnly) std::filesystem::create_directories(directory, error);
        if (error) return std::unexpected(storageError(error.message()));
        canonical = std::filesystem::canonical(directory, error);
        if (error) return std::unexpected(storageError(error.message()));
        const auto native = canonical.native();
        auto &pool = registry();
        const auto lock = scoped_lock(pool.gate);
        std::call_once(pool.initialization, [&] {
            MMKV::initializeMMKV(native, MMKVLogError);
            pool.initialized = true;
        });
        if (const auto found = pool.stores.find(StoreKey{canonical, identifier}); found != pool.stores.end()) {
            if (found->second.readOnly != readOnly)
                return std::unexpected(storageError("The store is already open in a different access mode."));
            ++found->second.references;
            store = found->second.store;
            operations = found->second.operations;
            return {};
        }
        if (readOnly && !std::filesystem::exists(canonical / identifier, error))
            return std::unexpected(storageError("The read-only MMKV store does not exist."));
        if (error) return std::unexpected(storageError(error.message()));
        if (auto valid = validateStore(canonical, identifier); !valid) return valid;
        auto config = MMKVConfig{};
        config.mode = readOnly ? MMKV_SINGLE_PROCESS | MMKV_READ_ONLY : MMKV_SINGLE_PROCESS;
        config.rootPath = &native;
        store = MMKV::mmkvWithID(identifier, config);
        if (!store) return std::unexpected(storageError("Cannot open the MMKV store."));
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
            return std::unexpected(storageError("The MMKV directory is unavailable."));
        return validateStore(canonical, identifier);
    }

    template <class T, class Operation>
    auto access(Operation operation, bool writing = false) const -> Result<T>
    {
        const auto lock = scoped_lock(gate);
        if (writing && readOnly)
            return std::unexpected(storageError("The MMKV store is read-only."));
        if (auto opened = open(); !opened) return std::unexpected(opened.error());
        const auto serialized = scoped_lock(operations->gate);
        if (auto valid = validate(); !valid) return std::unexpected(valid.error());
        auto result = operation(*store);
        // MMKV's own changes are trusted; outside edits show a new size or modification time.
        operations->verified = fingerprint(canonical, identifier);
        return result;
    }
    template <class T, class Getter>
    auto read(std::string_view key, Getter getter) const -> Result<std::optional<T>>
    {
        if (key.empty()) return std::unexpected(Error{ErrorCode::invalidKey, "An MMKV key is required."});
        return access<std::optional<T>>([&](MMKV &handle) -> Result<std::optional<T>> {
            if (!handle.containsKey(key)) return std::nullopt;
            T value{};
            if (!getter(handle, key, value))
                return std::unexpected(formatError("Cannot decode the MMKV value using the requested type."));
            return std::optional<T>{std::move(value)};
        });
    }
    template <class Setter>
    auto write(std::string_view key, Setter setter) const -> Result<void>
    {
        if (key.empty()) return std::unexpected(Error{ErrorCode::invalidKey, "An MMKV key is required."});
        return access<void>([&](MMKV &handle) -> Result<void> {
            if (!setter(handle, key)) return std::unexpected(storageError("MMKV could not save the value."));
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
        auto buffer = mmkv::MMBuffer{};
        if (!handle.getBytes(key, buffer)) return false;
        value.resize(buffer.length());
        if (!value.empty()) std::memcpy(value.data(), buffer.getPtr(), buffer.length());
        return true;
    });
}
auto MmkvStore::setBytes(std::string_view key, span<const std::byte> value) const -> Result<void>
{
    return m_impl->write(key, [value](MMKV &handle, auto key) {
        auto buffer = mmkv::MMBuffer(value.size());
        if (!value.empty()) std::memcpy(buffer.getPtr(), value.data(), value.size());
        return handle.set(buffer, key);
    });
}
auto MmkvStore::contains(std::string_view key) const -> Result<bool>
{
    if (key.empty()) return std::unexpected(Error{ErrorCode::invalidKey, "An MMKV key is required."});
    return m_impl->access<bool>([key](MMKV &handle) -> Result<bool> { return handle.containsKey(key); });
}
auto MmkvStore::keys() const -> Result<Strings>
{
    return m_impl->access<Strings>([](MMKV &handle) -> Result<Strings> { return handle.allKeys(); });
}
auto MmkvStore::remove(std::string_view key) const -> Result<void>
{
    return m_impl->write(key, [](MMKV &handle, auto key) {
        return !handle.containsKey(key) || handle.removeValueForKey(key);
    });
}
auto MmkvStore::clear() const -> Result<void>
{
    return m_impl->access<void>([](MMKV &handle) -> Result<void> {
        handle.clearAll();
        handle.sync(MMKV_SYNC);
        if (handle.count() != 0) return std::unexpected(storageError("MMKV could not clear the store."));
        return {};
    }, true);
}
} // namespace storage
