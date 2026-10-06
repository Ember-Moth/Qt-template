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
using std::size_t;
using std::uint32_t;
using std::uint16_t;

namespace business {
namespace {
struct CodePoint
{
    char32_t value;
    size_t begin;
    size_t end;
};

using CodePoints = vector<CodePoint>;
using DecodeResult = expected<CodePoints, Error>;

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
        else if (first >= 0x80) return std::unexpected(Error{ErrorCode::invalidTitle, "Invalid UTF-8."});
        for (int index = 0; index < continuation; ++index) {
            if (offset >= text.size())
                return std::unexpected(Error{ErrorCode::invalidTitle, "Incomplete UTF-8."});
            const auto next = static_cast<unsigned char>(text[offset++]);
            if ((next & 0xc0) != 0x80)
                return std::unexpected(Error{ErrorCode::invalidTitle, "Invalid UTF-8 continuation."});
            value = (value << 6) | (next & 0x3f);
        }
        if ((continuation == 1 && value < 0x80) || (continuation == 2 && value < 0x800)
            || (continuation == 3 && value < 0x10000) || value > 0x10ffff
            || (value >= 0xd800 && value <= 0xdfff))
            return std::unexpected(Error{ErrorCode::invalidTitle, "Invalid UTF-8 code point."});
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
} // namespace

auto normalizeTitle(string_view title) -> TitleResult
{
    auto decoded = decode(title);
    if (!decoded)
        return std::unexpected(decoded.error());
    const auto first = std::ranges::find_if_not(*decoded, [](const auto &p) { return whitespace(p.value); });
    const auto last = std::find_if_not(decoded->rbegin(), decoded->rend(),
                                      [](const auto &p) { return whitespace(p.value); }).base();
    if (first == decoded->end())
        return std::unexpected(Error{ErrorCode::invalidTitle, "A title is required."});
    size_t units = 0;
    for (auto point = first; point != last; ++point)
        units += point->value > 0xffff ? 2 : 1;
    if (units > Task::maxTitleLength)
        return std::unexpected(Error{ErrorCode::invalidTitle, "Title is too long."});
    return string(title.substr(first->begin, std::prev(last)->end - first->begin));
}

SaveResult validateTasks(span<const Task> tasks)
{
    auto ids = unordered_set<string>{};
    for (const auto &task : tasks) {
        const auto id = decode(task.id);
        const auto title = decode(task.title);
        const auto units = title ? title->size()
            + static_cast<size_t>(std::ranges::count_if(*title, [](const auto &p) { return p.value > 0xffff; }))
            : Task::maxTitleLength + 1;
        if (!id || std::ranges::all_of(*id, [](const auto &p) { return whitespace(p.value); })
            || !title || units > Task::maxTitleLength
            || std::ranges::all_of(*title, [](const auto &p) { return whitespace(p.value); })
            || !ids.insert(task.id).second)
            return std::unexpected(Error{ErrorCode::invalidRecord, "Invalid or duplicate task record."});
    }
    return {};
}

string createTaskId()
{
    thread_local auto generator = mt19937_64{random_device{}()};
    const auto first = generator();
    const auto second = generator();
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
        static_cast<uint32_t>(first >> 32), static_cast<uint16_t>(first >> 16),
        static_cast<uint16_t>((first & 0x0fff) | 0x4000),
        static_cast<uint16_t>((second >> 48 & 0x3fff) | 0x8000),
        second & 0xffffffffffffULL);
}
} // namespace business
