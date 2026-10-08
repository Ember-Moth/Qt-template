module;
#include "runtime/task.h"

module Template.Network.Http;
import :sdk;
import std;

using std::string;
// Same standard type; avoid clangd merging duplicate aliases from Asio headers and the std BMI.
using string_view = std::basic_string_view<char>;
using std::shared_ptr;
using std::format;
using errors::Err;
using std::make_shared;
using std::unreachable;
using runtime::Task;
using asio::io_context;

namespace network {
namespace {
auto methodName(Method method) -> string_view
{
    switch (method) {
    case Method::get: return "GET";
    case Method::post: return "POST";
    case Method::put: return "PUT";
    case Method::del: return "DELETE";
    }
    unreachable();
}
auto transferMethod(Method method) -> sdk::Method
{
    switch (method) {
    case Method::get: return sdk::Method::GET;
    case Method::post: return sdk::Method::POST;
    case Method::put: return sdk::Method::PUT;
    case Method::del: return sdk::Method::DEL;
    }
    unreachable();
}
} // namespace

struct HttpClient::Impl
{
    shared_ptr<sdk::Transport> transport;

    static auto send(shared_ptr<sdk::Transport> transport, Request request) -> Task<Result<Response>>
    {
        const auto context = format("{} {}", methodName(request.method), request.url);
        auto done = co_await sdk::Transport::perform(std::move(transport), {
            .method = transferMethod(request.method),
            .url = std::move(request.url),
            .headers = std::move(request.headers),
            .body = std::move(request.body),
            .timeout = request.timeout,
        });
        const auto failure = [&context](ErrorCode code, string_view reason) { return Err(code, "{}: {}", context, reason); };
        switch (done.outcome) {
        case sdk::Outcome::responded:
            co_return Response{.status = done.status, .headers = std::move(done.headers), .body = std::move(done.body)};
        case sdk::Outcome::failed: co_return failure(ErrorCode::transport, done.reason);
        case sdk::Outcome::cancelled: co_return failure(ErrorCode::cancelled, "the request was cancelled");
        case sdk::Outcome::stopped: co_return failure(ErrorCode::cancelled, "the HTTP client stopped");
        case sdk::Outcome::refused: co_return failure(ErrorCode::cancelled, "the HTTP client is stopped");
        }
        unreachable();
    }
};

HttpClient::HttpClient(io_context &context) : m_impl(make_shared<Impl>(make_shared<sdk::Transport>(context))) {}
HttpClient::~HttpClient() { sdk::Transport::release(std::move(m_impl->transport)); }
auto HttpClient::send(Request request) -> Task<Result<Response>>
{
    return Impl::send(m_impl->transport, std::move(request));
}
auto HttpClient::get(string url) -> Task<Result<Response>>
{
    return Impl::send(m_impl->transport, Request{.url = std::move(url)});
}
void HttpClient::stop() { sdk::Transport::stop(m_impl->transport); }
} // namespace network
