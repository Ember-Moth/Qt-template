module;
// Keep glaze and the standard headers it needs in a global module fragment, apart from the units that
// import std.
#include <glaze/json/read.hpp>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

module Template.Tasks.Import:json;

// Private implementation partition: the public import interface exports none of these names.
namespace business::json {
using std::string;
using std::string_view;
using std::vector;
using std::size_t;
using glz::opts;
using glz::read;
using glz::format_error;

// The fields read from each remote task; glaze matches the members by name and skips the others.
// glaze reflects only types with linkage, so this is not in an anonymous namespace.
struct RemoteTask
{
    string title;
    bool completed = false;
};
struct ReadError
{
    // Bytes of input read before the error.
    size_t offset = 0;
    string reason;
};

// Fills tasks, or error when the text is not a JSON array of tasks.
auto readTasks(string_view text, vector<RemoteTask> &tasks, ReadError &error) -> bool
{
    constexpr auto options = opts{.error_on_unknown_keys = false};
    if (const auto failure = read<options>(tasks, text)) {
        error = {failure.count, format_error(failure)};
        return false;
    }
    return true;
}
} // namespace business::json
