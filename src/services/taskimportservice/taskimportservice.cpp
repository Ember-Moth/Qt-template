module;
#include "runtime/task.h"
#include <asio/co_spawn.hpp>
#include <asio/strand.hpp>
#include <asio/use_awaitable.hpp>

module Template.Tasks.Import;
import :json;
import std;
import Template.Network.Http;

using std::string;
// Same standard type; avoid clangd merging duplicate aliases from Asio headers and the std BMI.
using string_view = std::basic_string_view<char>;
using std::shared_ptr;
using std::size_t;
using std::vector;
using std::format;
using std::make_shared;
using asio::co_spawn;
using asio::make_strand;
using asio::use_awaitable;
using runtime::Task;
using runtime::Executor;
using network::HttpClient;
using network::Response;
using errors::Err;

// Rust's impl From<network::Error> for business::Error: a network failure keeps its detail, and `?`
// converts it on the way out.
template <>
struct errors::From<business::ErrorCode, network::ErrorCode>
{
    static auto code(network::ErrorCode source) -> business::ErrorCode
    {
        return source == network::ErrorCode::cancelled ? business::ErrorCode::cancelled : business::ErrorCode::network;
    }
};

namespace business {
namespace {
// context names the request, as in "import tasks: GET <url>".
auto decodeTasks(string_view text, string_view context) -> ImportResult
{
    auto remote = vector<json::RemoteTask>{};
    auto failure = json::ReadError{};
    if (!json::readTasks(text, remote, failure))
        co_return Err(ErrorCode::invalidFormat, "{}: JSON at byte {}: {}", context, failure.offset, failure.reason);
    auto tasks = Tasks{};
    tasks.reserve(remote.size());
    for (auto index = size_t{0}; index < remote.size(); ++index) {
        auto title = co_await normalizeTitle(remote[index].title).context(format("{}: task {}", context, index));
        tasks.push_back({.id = createTaskId(), .title = std::move(title), .completed = remote[index].completed});
    }
    co_return tasks;
}
// The synchronous half of an import: the fetched response, or the failure that replaced it.
auto importTasks(network::Result<Response> fetched, string_view url) -> ImportResult
{
    const auto context = "import tasks";
    const auto response = co_await std::move(fetched).context(context);
    const auto request = format("{}: GET {}", context, url);
    if (!response.successful()) co_return Err(ErrorCode::network, "{}: HTTP {}", request, response.status);
    co_return decodeTasks(response.body, request);
}
} // namespace

struct TaskImportService::Impl
{
    shared_ptr<HttpClient> http;
    Executor strand;
    Impl(Executor executor, shared_ptr<HttpClient> client) : http(std::move(client))
    {
        if (!executor)
            throw Exception({ErrorCode::missingDependency, "TaskImportService: an Asio executor is required"});
        if (!http)
            throw Exception({ErrorCode::missingDependency, "TaskImportService: an HTTP client is required"});
        strand = make_strand(std::move(executor));
    }
    // Runs operation on the strand and resumes the caller on its own executor. The frame keeps the
    // shared state alive even if the service handle is gone before the operation finishes.
    static auto onStrand(shared_ptr<Impl> self, Task<ImportResult> operation) -> Task<ImportResult>
    {
        co_return co_await co_spawn(self->strand, std::move(operation), use_awaitable);
    }
    // Awaits only the request; HttpClient resumes this coroutine on the strand.
    auto fetchTasks(string url) -> Task<ImportResult>
    {
        auto response = co_await http->get(url);
        co_return importTasks(std::move(response), url);
    }
};

TaskImportService::TaskImportService(Executor executor, shared_ptr<HttpClient> http)
    : m_impl(make_shared<Impl>(std::move(executor), std::move(http))) {}
TaskImportService::~TaskImportService() = default;
auto TaskImportService::fetchTasks(string url) -> Task<ImportResult>
{
    return Impl::onStrand(m_impl, m_impl->fetchTasks(std::move(url)));
}
} // namespace business
