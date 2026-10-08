export module Template.Storage.Mmkv;
import std;
export import Template.Errors;

using std::span;
using std::unique_ptr;
using std::string;
using std::string_view;
using std::vector;
using std::optional;
using std::int64_t;
using std::uint64_t;
using std::byte;
using std::filesystem::path;

export namespace storage {
enum class ErrorCode { invalidIdentifier, invalidKey, io, invalidFormat };
using Error = errors::Error<ErrorCode>;
template <class T> using Result = errors::Result<T, ErrorCode>;
using Bytes = vector<byte>;
using Strings = vector<string>;

class MmkvStore
{
public:
    explicit MmkvStore(path directory, string identifier = "app", bool readOnly = false);
    ~MmkvStore();

    // Missing keys return an empty optional; callers read with the type they wrote.
    auto getBool(string_view key) const -> Result<optional<bool>>;
    auto getInt64(string_view key) const -> Result<optional<int64_t>>;
    auto getUInt64(string_view key) const -> Result<optional<uint64_t>>;
    auto getDouble(string_view key) const -> Result<optional<double>>;
    auto getString(string_view key) const -> Result<optional<string>>;
    auto getBytes(string_view key) const -> Result<optional<Bytes>>;
    auto getStrings(string_view key) const -> Result<optional<Strings>>;

    auto setBool(string_view key, bool value) const -> Result<void>;
    auto setInt64(string_view key, int64_t value) const -> Result<void>;
    auto setUInt64(string_view key, uint64_t value) const -> Result<void>;
    auto setDouble(string_view key, double value) const -> Result<void>;
    auto setString(string_view key, string_view value) const -> Result<void>;
    auto setBytes(string_view key, span<const byte> value) const -> Result<void>;
    auto setStrings(string_view key, span<const string> value) const -> Result<void>;
    auto contains(string_view key) const -> Result<bool>;
    auto keys() const -> Result<Strings>;
    auto remove(string_view key) const -> Result<void>;
    auto clear() const -> Result<void>;

private:
    struct Impl;
    unique_ptr<Impl> m_impl;
};
} // namespace storage
