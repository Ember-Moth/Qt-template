module Template.Models;
import std;

using std::string;
using std::string_view;
using std::vector;
using std::span;
using std::expected;
using std::unordered_set;
using std::mt19937_64;
using std::random_device;
using std::seed_seq;
using std::size_t;
using std::uint32_t;
using std::uint16_t;
using std::format;
using std::unexpected;
using errors::Ok;
using errors::Err;
using std::prev;
using std::ranges::all_of;
using std::ranges::find_if_not;

namespace business {
namespace {
struct CodePoint
{
    char32_t value;
    size_t begin;
    size_t end;
};

using CodePoints = vector<CodePoint>;
// Failures carry only the reason; callers add the context and choose the error code.
using DecodeResult = expected<CodePoints, string>;

auto decode(string_view text) -> DecodeResult
{
    auto points = CodePoints{};
    for (size_t offset = 0; offset < text.size();) {
        const auto begin = offset;
        const auto first = static_cast<unsigned char>(text[offset++]);
        char32_t value = first;
        int continuation = 0;
        if (first >= 0xc2 && first <= 0xdf) { value = first & 0x1f; continuation = 1; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 0x0f; continuation = 2; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 0x07; continuation = 3; }
        else if (first >= 0x80) return unexpected(format("invalid UTF-8 lead byte at offset {}", begin));
        for (int index = 0; index < continuation; ++index) {
            if (offset >= text.size())
                return unexpected(format("incomplete UTF-8 sequence at offset {}", begin));
            const auto next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xc0) != 0x80)
                return unexpected(format("invalid UTF-8 continuation byte at offset {}", offset - 1));
            value = (value << 6) | (next & 0x3f);
        }
        if ((continuation == 1 && value < 0x80) || (continuation == 2 && value < 0x800)
            || (continuation == 3 && value < 0x10000) || value > 0x10ffff
            || (value >= 0xd800 && value <= 0xdfff))
            return unexpected(format("invalid UTF-8 code point at offset {}", begin));
        points.push_back({value, begin, offset});
    }
    return points;
}

bool whitespace(char32_t value)
{
    return (value >= 0x09 && value <= 0x0d) || value == 0x20 || value == 0x85
        || value == 0xa0 || value == 0x1680 || (value >= 0x2000 && value <= 0x200a)
        || value == 0x2028 || value == 0x2029 || value == 0x202f || value == 0x205f
        || value == 0x3000;
}
bool blank(const CodePoints &points)
{
    return all_of(points, [](const auto &point) { return whitespace(point.value); });
}
auto utf16Units(auto first, auto last) -> size_t
{
    auto units = size_t{0};
    for (; first != last; ++first)
        units += first->value > 0xffff ? 2 : 1;
    return units;
}
} // namespace

auto normalizeTitle(string_view title) -> TitleResult
{
    const auto reject = [](string_view reason) { return Err(ErrorCode::invalidTitle, "task title: {}", reason); };
    auto decoded = decode(title);
    if (!decoded)
        return reject(decoded.error());
    const auto first = find_if_not(*decoded, [](const auto &p) { return whitespace(p.value); });
    const auto last = find_if_not(decoded->rbegin(), decoded->rend(),
                                      [](const auto &p) { return whitespace(p.value); }).base();
    if (first == decoded->end())
        return reject("a title is required");
    if (const auto units = utf16Units(first, last); units > TaskRecord::maxTitleLength)
        return reject(format("{} UTF-16 code units exceed the limit of {}", units, TaskRecord::maxTitleLength));
    return string(title.substr(first->begin, prev(last)->end - first->begin));
}

SaveResult validateTasks(span<const TaskRecord> tasks)
{
    auto ids = unordered_set<string>{};
    for (size_t index = 0; index < tasks.size(); ++index) {
        const auto &task = tasks[index];
        const auto reject = [&](string_view reason) {
            return Err(ErrorCode::invalidRecord, "task record {} (id '{}'): {}", index, task.id, reason);
        };
        const auto id = decode(task.id);
        if (!id)
            return reject(format("id has {}", id.error()));
        if (blank(*id))
            return reject("id is empty");
        const auto title = decode(task.title);
        if (!title)
            return reject(format("title has {}", title.error()));
        if (blank(*title))
            return reject("title is empty");
        if (const auto units = utf16Units(title->begin(), title->end()); units > TaskRecord::maxTitleLength)
            return reject(format("title has {} UTF-16 code units, more than {}", units, TaskRecord::maxTitleLength));
        if (!ids.insert(task.id).second)
            return reject("id is used by an earlier record");
    }
    return Ok();
}

string createTaskId()
{
    // A single 32-bit seed can repeat a whole ID sequence across launches.
    thread_local auto generator = [] {
        auto device = random_device{};
        auto seeds = seed_seq{device(), device(), device(), device(), device(), device(), device(), device()};
        return mt19937_64{seeds};
    }();
    const auto first = generator();
    const auto second = generator();
    return format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        static_cast<uint32_t>(first >> 32), static_cast<uint16_t>(first >> 16),
        static_cast<uint16_t>((first & 0x0fff) | 0x4000),
        static_cast<uint16_t>((second >> 48 & 0x3fff) | 0x8000),
        second & 0xffffffffffffULL);
}
} // namespace business
