export module Template.Models;
import std;

using std::string;
using std::string_view;
using std::vector;
using std::span;
using std::expected;
using std::size_t;

export namespace business {
struct Task
{
    static constexpr size_t maxTitleLength = 120;
    string id;
    string title;
    bool completed = false;
    bool operator==(const Task &) const = default;
};

enum class ErrorCode
{
    // Returned through std::expected.
    invalidTitle,
    invalidRecord,
    notFound,
    notReady,
    storage,
    invalidFormat,
    // Thrown as Exception: an object cannot be constructed or a caller broke a usage contract.
    missingDependency,
    contractViolation,
};

// detail reads "<context>: <reason>", outermost context first.
struct Error
{
    ErrorCode code;
    string detail;
};
auto withContext(Error error, string_view context) -> Error;

// Thrown only for construction failures and unrecoverable contract violations.
class Exception : public std::exception
{
public:
    explicit Exception(Error error) : m_error(std::move(error)) {}
    auto error() const noexcept -> const Error & { return m_error; }
    auto what() const noexcept -> const char * override { return m_error.detail.c_str(); }

private:
    Error m_error;
};

using Tasks = vector<Task>;
using TitleResult = expected<string, Error>;
using TaskResult = expected<Tasks, Error>;
using SaveResult = expected<void, Error>;

// Text at the business boundary is UTF-8; limits count UTF-16 code units.
auto normalizeTitle(string_view title) -> TitleResult;
SaveResult validateTasks(span<const Task> tasks);
string createTaskId();
} // namespace business
