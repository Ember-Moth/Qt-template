export module Template.Errors;
import std;

using std::string;
using std::string_view;
using std::expected;
using std::unexpected;
using std::optional;
using std::format;
using std::format_string;
using std::exception;
using std::coroutine_handle;
using std::suspend_never;
using std::suspend_always;
using std::is_void_v;
using std::invoke_result_t;
using std::same_as;
using std::uncaught_exceptions;
using std::exchange;

// Rust-style error handling shared by every layer. A layer keeps only its own `enum class ErrorCode`
// and names the rest, as a Rust module names `pub type Result<T> = …`:
//
//     using Error = errors::Error<ErrorCode>;
//     template <class T> using Result = errors::Result<T, ErrorCode>;
//     using Exception = errors::Exception<ErrorCode>;
//
// A function returning Result reads like Rust:
//
//     auto add(string title) -> Result<Update>
//     {
//         auto normalized = co_await normalizeTitle(title).context("add task");  // normalizeTitle(..)?
//         if (normalized.empty()) co_return Err(ErrorCode::invalidTitle, "add task: empty");
//         co_return Ok(Update{…});
//     }
//
// co_await on a Result is Rust's `?`: the value when it succeeded, otherwise the function returns that
// error at once, converted through From when it comes from another layer. Asio coroutines (Task<T>)
// await only asynchronous work; they hand Result logic to such synchronous functions.
export namespace errors {
// Rust's From between error types: specialize it to turn another layer's codes into Code.
//     template <> struct errors::From<business::ErrorCode, storage::ErrorCode>
//     {
//         static auto code(storage::ErrorCode source) -> business::ErrorCode { … }
//     };
template <class Code, class Source> struct From;
template <class Code, class Source>
concept Converts = requires(Source source) {
    { From<Code, Source>::code(source) } -> same_as<Code>;
};

// One failure: what went wrong (the layer's code) and why. detail reads "<context>: <reason>", the
// outermost context first.
template <class Code>
struct Error
{
    Code code;
    string detail;

    Error(Code failure, string reason) : code(failure), detail(std::move(reason)) {}
    // Another layer's failure under this layer's code, keeping its detail; implicit, as `?` converts.
    template <class Source>
        requires Converts<Code, Source>
    Error(Error<Source> source) : code(From<Code, Source>::code(source.code)), detail(std::move(source.detail)) {}

    // Adds an outer context, as anyhow's .context() does: "load tasks" before "read 'k': io".
    auto context(string_view outer) && -> Error
    {
        detail = format("{}: {}", outer, detail);
        return std::move(*this);
    }
    auto context(string_view outer) const & -> Error { return Error(*this).context(outer); }
};

// Thrown only when an object cannot be constructed or a caller breaks a usage contract; recoverable
// failures return Result instead.
template <class Code>
class Exception : public exception
{
public:
    explicit Exception(Error<Code> error) : m_error(std::move(error)) {}
    auto error() const noexcept -> const Error<Code> & { return m_error; }
    auto what() const noexcept -> const char * override { return m_error.detail.c_str(); }

private:
    Error<Code> m_error;
};

// Rust's (): what Ok() holds for a Result<void>.
struct Unit
{
};

// Rust's Result<T, E>, built on std::expected: has_value(), value(), operator* and the std::expected
// combinators still work. [[nodiscard]] plays the part of Rust's #[must_use].
template <class T, class Code>
class [[nodiscard]] Result : public expected<T, Error<Code>>
{
    using Base = expected<T, Error<Code>>;

public:
    using Base::Base;
    Result() = default;
    Result(Base result) : Base(std::move(result)) {}
    Result(Unit)
        requires is_void_v<T>
    {
    }

    auto is_ok() const noexcept -> bool { return this->has_value(); }
    auto is_err() const noexcept -> bool { return !this->has_value(); }

    // anyhow's .context(): a failure gains an outer context; a value passes through.
    auto context(string_view outer) && -> Result
    {
        if (is_err()) return unexpected(std::move(*this).error().context(outer));
        return std::move(*this);
    }
    auto context(string_view outer) const & -> Result { return Result(*this).context(outer); }

    // map: a Result of function(value); a failure passes through.
    template <class Function>
    auto map(Function function) &&
    {
        if constexpr (is_void_v<T>) {
            using U = invoke_result_t<Function>;
            if (is_err()) return Result<U, Code>(unexpected(std::move(*this).error()));
            if constexpr (is_void_v<U>) {
                function();
                return Result<U, Code>();
            } else {
                return Result<U, Code>(function());
            }
        } else {
            using U = invoke_result_t<Function, T>;
            if (is_err()) return Result<U, Code>(unexpected(std::move(*this).error()));
            if constexpr (is_void_v<U>) {
                function(std::move(**this));
                return Result<U, Code>();
            } else {
                return Result<U, Code>(function(std::move(**this)));
            }
        }
    }
    // map_err: function turns the error into another Error; a value passes through.
    template <class Function>
    auto map_err(Function function) &&
    {
        using Mapped = invoke_result_t<Function, Error<Code>>;
        using Target = Result<T, decltype(Mapped::code)>;
        if (is_err()) return Target(unexpected(function(std::move(*this).error())));
        if constexpr (is_void_v<T>) return Target();
        else return Target(std::move(**this));
    }
    // and_then: chains a function that returns its own Result; a failure passes through, converted
    // through From when the next Result belongs to another layer.
    template <class Function>
    auto and_then(Function function) &&
    {
        if constexpr (is_void_v<T>) {
            using Next = invoke_result_t<Function>;
            if (is_err()) return Next(unexpected(std::move(*this).error()));
            return function();
        } else {
            using Next = invoke_result_t<Function, T>;
            if (is_err()) return Next(unexpected(std::move(*this).error()));
            return function(std::move(**this));
        }
    }
    // unwrap_or: the value, or fallback after a failure.
    template <class U = T>
        requires(!is_void_v<U>)
    auto unwrap_or(U fallback) && -> U
    {
        return is_ok() ? std::move(**this) : std::move(fallback);
    }
    // expect: the value, or Rust's panic as an Exception whose detail starts with message. Use it
    // where a failure means a broken contract, as in tests.
    auto expect(string_view message) &&
    {
        if (is_err()) throw Exception<Code>(std::move(*this).error().context(message));
        if constexpr (!is_void_v<T>) return std::move(**this);
    }

    class Awaiter;
    class promise_type;
    // Rust's `?` inside a function that returns a Result; see the module comment.
    auto operator co_await() && -> Awaiter { return Awaiter(std::move(*this)); }
    auto operator co_await() const & -> Awaiter { return Awaiter(*this); }
};

template <class T, class Code>
class Result<T, Code>::Awaiter
{
public:
    explicit Awaiter(Result result) : m_result(std::move(result)) {}
    auto await_ready() const noexcept -> bool { return m_result.is_ok(); }
    // A failure ends the awaiting Result coroutine with this error; its frame is destroyed once the
    // caller has the Result. Asio coroutines reject this awaiter at compile time.
    template <class Promise>
    void await_suspend(coroutine_handle<Promise> handle)
    {
        handle.promise().fail(std::move(m_result).error());
    }
    auto await_resume() -> T
    {
        if constexpr (!is_void_v<T>) return std::move(*m_result);
    }

private:
    Result m_result;
};

// Runs a Result coroutine eagerly to its end or to its first failed `?`. Clang converts the return
// object only when the coroutine first returns to its caller, so the result is ready by then.
template <class T, class Code>
class Result<T, Code>::promise_type
{
public:
    class Pending
    {
    public:
        explicit Pending(coroutine_handle<promise_type> handle) : m_handle(handle) {}
        Pending(Pending &&other) noexcept : m_handle(exchange(other.m_handle, {})), m_unwinding(other.m_unwinding) {}
        Pending &operator=(Pending &&) = delete;
        // An exception that leaves the coroutine's first run destroys the frame on its own.
        ~Pending()
        {
            if (m_handle && uncaught_exceptions() == m_unwinding) m_handle.destroy();
        }
        operator Result() { return std::move(*m_handle.promise().m_result); }

    private:
        coroutine_handle<promise_type> m_handle;
        int m_unwinding = uncaught_exceptions();
    };

    auto get_return_object() -> Pending { return Pending(coroutine_handle<promise_type>::from_promise(*this)); }
    auto initial_suspend() noexcept -> suspend_never { return {}; }
    auto final_suspend() noexcept -> suspend_always { return {}; }
    void return_value(Result result) { m_result.emplace(std::move(result)); }
    // Exceptions are unrecoverable here as everywhere else; they leave through the caller.
    void unhandled_exception() { throw; }
    template <class Source>
    void fail(Error<Source> error)
    {
        m_result.emplace(unexpected(Error<Code>(std::move(error))));
    }

private:
    optional<Result> m_result;
};

// Rust's Ok: co_return Ok(value); or co_return Ok(); in a Result<void> function.
template <class T>
auto Ok(T value) -> T
{
    return value;
}
inline auto Ok() -> Unit { return {}; }

// Rust's Err: co_return Err(ErrorCode::notFound, "{}: no task has this id", context);
template <class Code, class... Args>
auto Err(Code code, format_string<Args...> reason, Args &&...arguments) -> unexpected<Error<Code>>
{
    return unexpected(Error<Code>(code, format(reason, std::forward<Args>(arguments)...)));
}
template <class Code>
auto Err(Error<Code> error) -> unexpected<Error<Code>>
{
    return unexpected(std::move(error));
}
} // namespace errors
