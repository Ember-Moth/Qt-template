import std;
import Template.Errors;

using std::string;
using std::string_view;
using std::source_location;
using std::runtime_error;
using std::exception;
using std::array;
using std::pair;
using errors::Ok;
using errors::Err;

// Two layers, as storage and business are: Low failures turn into High ones through From.
enum class Low { io, missing };
enum class High { storage, notFound, invalid };
template <>
struct errors::From<High, Low>
{
    static auto code(Low source) -> High { return source == Low::missing ? High::notFound : High::storage; }
};
template <class T> using LowResult = errors::Result<T, Low>;
template <class T> using HighResult = errors::Result<T, High>;

namespace {
void require(bool condition, source_location where = source_location::current())
{
    if (!condition)
        throw runtime_error(std::format("{}:{} check failed", where.file_name(), where.line()));
}

// Counts the locals of Result coroutines that are still alive.
int alive = 0;
struct Local
{
    Local() { ++alive; }
    Local(const Local &) = delete;
    ~Local() { --alive; }
};

auto readNumber(string_view text) -> LowResult<int>
{
    if (text.empty()) return Err(Low::missing, "nothing to read");
    if (text == "x") return Err(Low::io, "unreadable '{}'", text);
    return Ok(static_cast<int>(text.size()));
}
auto checkPositive(int value) -> HighResult<void>
{
    if (value <= 0) return Err(High::invalid, "{} is not positive", value);
    return Ok();
}
// Rust: fn double(text: &str) -> Result<i32> { let n = read_number(text).context("double")?; … }
auto doubled(string_view text) -> HighResult<int>
{
    Local local;
    const auto number = co_await readNumber(text).context("double");
    co_await checkPositive(number);
    co_return Ok(number * 2);
}
auto chained(string_view first, string_view second) -> HighResult<string>
{
    Local local;
    const auto a = co_await doubled(first);
    const auto b = co_await doubled(second).context("second");
    co_return std::format("{}+{}", a, b);
}
auto throwing() -> HighResult<int>
{
    Local local;
    co_await checkPositive(1);
    throw runtime_error("unrecoverable");
    co_return 0;
}
auto awaitsThrowing() -> HighResult<int>
{
    Local local;
    co_return co_await throwing();
}

void okAndErr()
{
    const auto ok = readNumber("abc");
    require(ok.is_ok() && !ok.is_err() && *ok == 3);
    const auto missing = readNumber("");
    require(missing.is_err() && missing.error().code == Low::missing && missing.error().detail == "nothing to read");
    const auto formatted = readNumber("x");
    require(formatted.error().detail == "unreadable 'x'");
    require(checkPositive(1).is_ok() && checkPositive(0).error().detail == "0 is not positive");
}

void questionMark()
{
    const auto value = doubled("abcd");
    require(value && *value == 8 && alive == 0);
    // A failed `?` returns at once, converted through From, with the context the call added.
    const auto missing = doubled("");
    require(!missing && missing.error().code == High::notFound && missing.error().detail == "double: nothing to read");
    const auto io = doubled("x");
    require(!io && io.error().code == High::storage);
    require(alive == 0);
    // A void Result works as `?` too.
    const auto chain = chained("ab", "abc");
    require(chain && *chain == "4+6");
    const auto second = chained("ab", "");
    require(!second && second.error().detail == "second: double: nothing to read" && alive == 0);
}

void context()
{
    auto error = errors::Error<Low>(Low::io, "disk");
    const auto outer = error.context("save");
    require(outer.detail == "save: disk" && error.detail == "disk");
    const auto moved = std::move(error).context("load").context("start");
    require(moved.detail == "start: load: disk");
    const auto failed = readNumber("").context("parse");
    require(failed.error().detail == "parse: nothing to read");
    const auto passed = readNumber("a").context("parse");
    require(passed && *passed == 1);
}

void from()
{
    // Implicit, as Rust converts with From.
    errors::Error<High> converted = errors::Error<Low>(Low::missing, "gone");
    require(converted.code == High::notFound && converted.detail == "gone");
    HighResult<int> result = readNumber("");
    require(!result && result.error().code == High::notFound);
    const auto returned = []() -> HighResult<void> { return Err(Low::io, "lost"); }();
    require(!returned && returned.error().code == High::storage);
}

void combinators()
{
    const auto mapped = readNumber("abc").map([](int value) { return value * 10; });
    require(mapped && *mapped == 30);
    const auto stillFailed = readNumber("").map([](int value) { return value * 10; });
    require(!stillFailed && stillFailed.error().code == Low::missing);
    const auto described = readNumber("").map_err([](errors::Error<Low> error) {
        return errors::Error<High>(High::invalid, std::format("bad input: {}", error.detail));
    });
    require(!described && described.error().code == High::invalid && described.error().detail == "bad input: nothing to read");
    const auto checked = readNumber("ab").and_then(checkPositive);
    require(checked.is_ok());
    const auto failedEarly = readNumber("").and_then(checkPositive);
    require(!failedEarly && failedEarly.error().code == High::notFound);
    require(readNumber("abc").unwrap_or(0) == 3 && readNumber("").unwrap_or(-1) == -1);
    require(readNumber("ab").expect("read") == 2);
    auto panicked = false;
    try {
        static_cast<void>(readNumber("").expect("read the count"));
    } catch (const errors::Exception<Low> &error) {
        panicked = error.error().code == Low::missing && string_view(error.what()) == "read the count: nothing to read";
    }
    require(panicked);
    // std::expected members remain available.
    require(readNumber("ab").value() == 2 && readNumber("").value_or(5) == 5);
}

void exceptions()
{
    auto caught = false;
    try {
        static_cast<void>(throwing());
    } catch (const runtime_error &error) {
        caught = string_view(error.what()) == "unrecoverable";
    }
    require(caught && alive == 0);
    caught = false;
    try {
        static_cast<void>(awaitsThrowing());
    } catch (const runtime_error &) {
        caught = true;
    }
    require(caught && alive == 0);
}
} // namespace

int main()
{
    const array tests{
        pair{"ok and err", &okAndErr}, pair{"question mark", &questionMark}, pair{"context", &context},
        pair{"from", &from}, pair{"combinators", &combinators}, pair{"exceptions", &exceptions},
    };
    for (const auto &[name, test] : tests) {
        try { test(); std::println("PASS {}", name); }
        catch (const exception &error) { std::println(std::cerr, "FAIL {}: {}", name, error.what()); return 1; }
    }
}
