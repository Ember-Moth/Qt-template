export module Template.Models;
import std;
export import Template.Errors;

using std::string;
using std::string_view;
using std::vector;
using std::span;
using std::size_t;

export namespace business {
struct TaskRecord
{
    static constexpr size_t maxTitleLength = 120;
    string id;
    string title;
    bool completed = false;
    bool operator==(const TaskRecord &) const = default;
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
    // A remote request failed in transport or answered with an unexpected HTTP status.
    network,
    // A remote request was cancelled, as when the application stops.
    cancelled,
    // Thrown as Exception: an object cannot be constructed or a caller broke a usage contract.
    missingDependency,
    contractViolation,
};

using Error = errors::Error<ErrorCode>;
template <class T> using Result = errors::Result<T, ErrorCode>;
// Thrown only for construction failures and unrecoverable contract violations.
using Exception = errors::Exception<ErrorCode>;

using Tasks = vector<TaskRecord>;
using TitleResult = Result<string>;
using TaskResult = Result<Tasks>;
using SaveResult = Result<void>;

// Text at the business boundary is UTF-8; limits count UTF-16 code units.
auto normalizeTitle(string_view title) -> TitleResult;
SaveResult validateTasks(span<const TaskRecord> tasks);
string createTaskId();
} // namespace business
