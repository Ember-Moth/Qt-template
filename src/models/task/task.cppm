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

enum class ErrorCode { invalidTitle, invalidRecord, notFound, notReady, storage, invalidFormat, internal };

struct Error
{
    ErrorCode code;
    string detail;
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
