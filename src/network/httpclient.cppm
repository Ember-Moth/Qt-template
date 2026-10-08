module;
#include "runtime/task.h"

export module Template.Network.Http;
import std;
export import Template.Errors;

using std::string;
using std::vector;
using std::shared_ptr;
using std::chrono::seconds;
using runtime::Task;
using asio::io_context;

export namespace network {
enum class ErrorCode { transport, cancelled };
// detail reads "<method> <url>: <reason>".
using Error = errors::Error<ErrorCode>;
template <class T> using Result = errors::Result<T, ErrorCode>;

enum class Method { get, post, put, del };
struct Request
{
    Method method = Method::get;
    string url;
    // Each entry is one "Name: value" line.
    vector<string> headers;
    string body;
    seconds timeout{10};
};
// Every HTTP status arrives as a Response; callers decide which statuses they accept.
struct Response
{
    int status = 0;
    string headers;
    string body;
    auto successful() const -> bool { return status >= 200 && status < 300; }
};

// libcurl transfers through cofetch, which runs every call and completion on the runtime's single
// worker thread. Transfers in flight keep the client's state alive.
class HttpClient
{
public:
    // The runtime's event loop (runtime::AsioRuntime::context()), which runs on one thread.
    explicit HttpClient(io_context &context);
    ~HttpClient();
    HttpClient(const HttpClient &) = delete;
    HttpClient &operator=(const HttpClient &) = delete;

    // Await from any coroutine: the transfer runs on the runtime thread and the caller resumes on its
    // own executor. Cancelling the awaiting coroutine aborts the transfer.
    auto send(Request request) -> Task<Result<Response>>;
    auto get(string url) -> Task<Result<Response>>;
    // Aborts the transfers in flight and refuses new ones; call before the io_context drains.
    void stop();

private:
    struct Impl;
    shared_ptr<Impl> m_impl;
};
} // namespace network
