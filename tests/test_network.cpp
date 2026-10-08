#include "runtime/task.h"
#include <asio/as_tuple.hpp>
#include <asio/bind_cancellation_slot.hpp>
#include <asio/buffer.hpp>
#include <asio/cancellation_signal.hpp>
#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/io_context.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/post.hpp>
#include <asio/read.hpp>
#include <asio/read_until.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

import std;
import Template.Network.Http;
import Template.Tasks.Import;
import Template.Runtime.Asio;

using std::string;
using std::string_view;
using std::vector;
using std::map;
using std::atomic;
using std::jthread;
using std::exception;
using std::source_location;
using std::runtime_error;
using std::array;
using std::pair;
using namespace std::chrono_literals;
using tcp = asio::ip::tcp;

namespace {
void require(bool condition, source_location where = source_location::current())
{
    if (!condition)
        throw runtime_error(std::format("{}:{} check failed", where.file_name(), where.line()));
}
template <class Error>
bool mentions(const Error &error, string_view context)
{
    return error.detail.find(context) != string::npos;
}
// Runs a task to completion from this thread, on a loop of its own, as a caller outside the runtime.
template <class T>
auto await(runtime::Task<T> task) -> T
{
    asio::io_context caller;
    auto future = asio::co_spawn(caller, std::move(task), asio::use_future);
    caller.run_for(10s);
    require(future.wait_for(0s) == std::future_status::ready);
    return future.get();
}

// A loopback HTTP/1.1 server with canned routes. /echo answers with the request's method and body;
// /hang reads the request and never answers.
class LocalServer
{
public:
    struct Route
    {
        int status = 200;
        string body;
    };
    explicit LocalServer(map<string, Route> routes = {}) : m_routes(std::move(routes))
    {
        asio::co_spawn(m_context, serve(), asio::detached);
        m_thread = jthread([this] { m_context.run(); });
    }
    ~LocalServer()
    {
        m_context.stop();
        m_thread.join();
    }
    auto url(string_view path) const -> string { return std::format("http://127.0.0.1:{}{}", m_port, path); }
    // Blocks until count requests for /hang have arrived in total.
    void waitForHangs(int count) const
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (m_hangs.load() < count) {
            require(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(5ms);
        }
    }

private:
    auto serve() -> asio::awaitable<void>
    {
        for (;;) {
            auto socket = co_await m_acceptor.async_accept(asio::use_awaitable);
            asio::co_spawn(m_context, answer(std::move(socket)), asio::detached);
        }
    }
    auto answer(tcp::socket socket) -> asio::awaitable<void>
    {
        auto request = string{};
        const auto [error, headerEnd] = co_await asio::async_read_until(
            socket, asio::dynamic_buffer(request), "\r\n\r\n", asio::as_tuple(asio::use_awaitable));
        if (error) co_return;
        // The request line reads "<method> <path> HTTP/1.1".
        const auto line = string_view(request).substr(0, request.find("\r\n"));
        const auto methodEnd = line.find(' ');
        const auto method = string(line.substr(0, methodEnd));
        const auto path = string(line.substr(methodEnd + 1, line.find(' ', methodEnd + 1) - methodEnd - 1));
        auto body = request.substr(headerEnd);
        if (const auto at = request.find("Content-Length: "); at != string::npos && at < headerEnd) {
            const auto length = std::stoul(request.substr(at + 16));
            if (body.size() < length) {
                auto rest = string(length - body.size(), '\0');
                co_await asio::async_read(socket, asio::buffer(rest), asio::as_tuple(asio::use_awaitable));
                body += rest;
            }
        }
        if (path == "/hang") {
            m_held.push_back(std::move(socket));
            ++m_hangs;
            co_return;
        }
        auto route = Route{404, "{}"};
        if (path == "/echo")
            route = {200, std::format("{} {}", method, body)};
        else if (const auto found = m_routes.find(path); found != m_routes.end())
            route = found->second;
        const auto response = std::format(
            "HTTP/1.1 {} Status\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
            route.status, route.body.size(), route.body);
        co_await asio::async_write(socket, asio::buffer(response), asio::as_tuple(asio::use_awaitable));
        std::error_code ignored;
        socket.shutdown(tcp::socket::shutdown_both, ignored);
    }

    asio::io_context m_context;
    tcp::acceptor m_acceptor{m_context, {asio::ip::address_v4::loopback(), 0}};
    unsigned short m_port = m_acceptor.local_endpoint().port();
    map<string, Route> m_routes;
    // Server-thread state: /hang connections stay open until the server stops.
    vector<tcp::socket> m_held;
    atomic<int> m_hangs = 0;
    jthread m_thread;
};
// A loopback URL nothing listens on.
auto refusedUrl() -> string
{
    asio::io_context context;
    tcp::acceptor acceptor{context, {asio::ip::address_v4::loopback(), 0}};
    const auto port = acceptor.local_endpoint().port();
    acceptor.close();
    return std::format("http://127.0.0.1:{}/refused", port);
}
auto resumesOn(asio::strand<runtime::Executor> strand, network::HttpClient &http, string url) -> runtime::Task<bool>
{
    const auto response = co_await http.get(std::move(url));
    co_return response.has_value() && strand.running_in_this_thread();
}
auto resumesOnThread(network::HttpClient &http, string url) -> runtime::Task<std::thread::id>
{
    const auto response = co_await http.get(std::move(url));
    require(response.has_value());
    co_return std::this_thread::get_id();
}

void requests()
{
    LocalServer server({{"/todos", {200, R"([{"title":"one"}])"}}, {"/down", {503, "down"}}});
    runtime::AsioRuntime asyncRuntime;
    network::HttpClient http(asyncRuntime.context());
    const auto strand = asio::make_strand(asyncRuntime.executor());

    const auto found = await(http.get(server.url("/todos")));
    require(found && found->successful() && found->status == 200 && found->body == R"([{"title":"one"}])"
        && found->headers.contains("Content-Type: application/json"));
    // An HTTP error status is a response, not a transport failure.
    const auto down = await(http.get(server.url("/down")));
    require(down && !down->successful() && down->status == 503 && down->body == "down");
    const auto missing = await(http.get(server.url("/missing")));
    require(missing && missing->status == 404);
    const auto posted = await(http.send({
        .method = network::Method::post,
        .url = server.url("/echo"),
        .headers = {"Content-Type: application/json"},
        .body = R"({"title":"new"})",
    }));
    require(posted && posted->body == R"(POST {"title":"new"})");

    // cofetch completes on the runtime thread; callers resume on their own executor.
    require(asio::co_spawn(strand, resumesOn(strand, http, server.url("/todos")), asio::use_future).get());
    asio::io_context callerContext;
    auto onCaller = asio::co_spawn(callerContext, resumesOnThread(http, server.url("/todos")), asio::use_future);
    jthread caller([&callerContext] { callerContext.run(); });
    require(onCaller.wait_for(5s) == std::future_status::ready && onCaller.get() == caller.get_id());
}

void failures()
{
    LocalServer server;
    runtime::AsioRuntime asyncRuntime;
    network::HttpClient http(asyncRuntime.context());
    const auto url = refusedUrl();
    const auto refused = await(http.get(url));
    require(!refused && refused.error().code == network::ErrorCode::transport
        && mentions(refused.error(), std::format("GET {}: ", url)));
    const auto slow = await(http.send({.url = server.url("/hang"), .timeout = 1s}));
    require(!slow && slow.error().code == network::ErrorCode::transport
        && mentions(slow.error(), std::format("GET {}: ", server.url("/hang"))));
}

void cancellation()
{
    LocalServer server;
    runtime::AsioRuntime asyncRuntime;
    network::HttpClient http(asyncRuntime.context());

    // Cancelling the awaiting coroutine aborts its transfer.
    asio::cancellation_signal signal;
    auto cancelled = asio::co_spawn(asyncRuntime.executor(), http.send({.url = server.url("/hang"), .timeout = 30s}),
        asio::bind_cancellation_slot(signal.slot(), asio::use_future));
    server.waitForHangs(1);
    asio::post(asyncRuntime.executor(), [&signal] { signal.emit(asio::cancellation_type::terminal); });
    require(cancelled.wait_for(5s) == std::future_status::ready);
    const auto aborted = cancelled.get();
    require(!aborted && aborted.error().code == network::ErrorCode::cancelled
        && mentions(aborted.error(), "the request was cancelled"));

    // stop() ends the transfers in flight without waiting for their timeouts, then refuses new ones.
    auto pending = asio::co_spawn(asyncRuntime.executor(), http.send({.url = server.url("/hang"), .timeout = 30s}),
        asio::use_future);
    server.waitForHangs(2);
    http.stop();
    require(pending.wait_for(5s) == std::future_status::ready);
    const auto stopped = pending.get();
    require(!stopped && stopped.error().code == network::ErrorCode::cancelled
        && mentions(stopped.error(), std::format("GET {}: the HTTP client stopped", server.url("/hang"))));
    const auto refused = await(http.get(server.url("/hang")));
    require(!refused && refused.error().code == network::ErrorCode::cancelled
        && mentions(refused.error(), "the HTTP client is stopped"));
    http.stop();
}

void lifetime()
{
    LocalServer server({{"/todos", {200, "[]"}}});
    runtime::AsioRuntime asyncRuntime;
    auto http = std::make_unique<network::HttpClient>(asyncRuntime.context());
    // A transfer created before the client handle is gone still completes.
    auto pending = asio::co_spawn(asyncRuntime.executor(), http->get(server.url("/todos")), asio::use_future);
    http.reset();
    require(pending.wait_for(5s) == std::future_status::ready);
    const auto response = pending.get();
    require(response && response->body == "[]");
}

void taskImport()
{
    LocalServer server({
        {"/tasks", {200, R"([{"userId":1,"id":1,"title":"  Read the docs  ","completed":false},)"
                         R"({"id":2,"title":"学习 QML","completed":true,"tags":["x"]}])"}},
        {"/broken", {200, R"([{"title":"x",)"}},
        {"/blank", {200, R"([{"title":"ok"},{"title":"   "}])"}},
        {"/down", {503, "{}"}},
    });
    runtime::AsioRuntime asyncRuntime;
    auto http = std::make_shared<network::HttpClient>(asyncRuntime.context());
    business::TaskImportService service(asyncRuntime.executor(), http);
    const auto fetch = [&](string_view path) { return await(service.fetchTasks(server.url(path))); };

    const auto imported = fetch("/tasks");
    require(imported && imported->size() == 2);
    require(imported->at(0).title == "Read the docs" && !imported->at(0).completed);
    require(imported->at(1).title == "学习 QML" && imported->at(1).completed);
    require(!imported->at(0).id.empty() && imported->at(0).id != imported->at(1).id);

    const auto broken = fetch("/broken");
    require(!broken && broken.error().code == business::ErrorCode::invalidFormat
        && mentions(broken.error(), std::format("import tasks: GET {}: JSON at byte", server.url("/broken"))));
    const auto blank = fetch("/blank");
    require(!blank && blank.error().code == business::ErrorCode::invalidTitle
        && mentions(blank.error(), std::format("import tasks: GET {}: task 1: task title", server.url("/blank"))));
    const auto down = fetch("/down");
    require(!down && down.error().code == business::ErrorCode::network
        && mentions(down.error(), std::format("import tasks: GET {}: HTTP 503", server.url("/down"))));
    const auto url = refusedUrl();
    const auto refused = await(service.fetchTasks(url));
    require(!refused && refused.error().code == business::ErrorCode::network
        && mentions(refused.error(), std::format("import tasks: GET {}: ", url)));

    auto missing = false;
    try {
        business::TaskImportService unusable(asyncRuntime.executor(), nullptr);
    } catch (const business::Exception &error) {
        missing = error.error().code == business::ErrorCode::missingDependency && mentions(error.error(), "HTTP client");
    }
    require(missing);

    // Stopping the HTTP client, as the application does on exit, cancels the import.
    auto pending = asio::co_spawn(asyncRuntime.executor(), service.fetchTasks(server.url("/hang")), asio::use_future);
    server.waitForHangs(1);
    http->stop();
    require(pending.wait_for(5s) == std::future_status::ready);
    const auto stopped = pending.get();
    require(!stopped && stopped.error().code == business::ErrorCode::cancelled
        && mentions(stopped.error(), std::format("import tasks: GET {}: the HTTP client stopped", server.url("/hang"))));
}
} // namespace

int main()
{
    const array tests{
        pair{"requests", &requests}, pair{"failures", &failures}, pair{"cancellation", &cancellation},
        pair{"lifetime", &lifetime}, pair{"task import", &taskImport},
    };
    for (const auto &[name, test] : tests) {
        try { test(); std::println("PASS {}", name); }
        catch (const exception &error) { std::println(std::cerr, "FAIL {}: {}", name, error.what()); return 1; }
    }
}
