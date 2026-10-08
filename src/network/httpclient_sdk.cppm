module;
// Keep cofetch, libcurl and the standard headers they need in a global module fragment, apart from the
// units that import std.
#include <asio/as_tuple.hpp>
#include <asio/co_spawn.hpp>
#include <asio/experimental/awaitable_operators.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>
#include <asio/steady_timer.hpp>
#include <asio/use_awaitable.hpp>
#include <cofetch.h>
#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

module Template.Network.Http:sdk;

// Private implementation partition: the public network interface exports none of these names.
namespace network::sdk {
using std::string;
using std::vector;
using std::shared_ptr;
using std::weak_ptr;
using std::get;
using std::chrono::seconds;
using asio::awaitable;
using asio::io_context;
using asio::steady_timer;
using asio::co_spawn;
using asio::post;
using asio::as_tuple;
using asio::use_awaitable;
using asio::error::operation_aborted;
using cofetch::Client;
using Request = cofetch::Request;
using Method = Request::Method;
struct Transfer
{
    Method method = Method::GET;
    string url;
    vector<string> headers;
    string body;
    seconds timeout{10};
};
enum class Outcome { responded, failed, cancelled, stopped, refused };
struct Completion
{
    Outcome outcome = Outcome::responded;
    // The transport failure for Outcome::failed.
    string reason;
    int status = 0;
    string headers;
    string body;
};

class Transport
{
public:
    explicit Transport(io_context &context)
        : m_context(context), m_client(context), m_stopSignal(context, steady_timer::time_point::max()) {}

    // cofetch is single-threaded and completes inline on its io_context, outside the caller's executor:
    // run the transfer there, and let co_spawn resume the caller on its own executor.
    static auto perform(shared_ptr<Transport> self, Transfer transfer) -> awaitable<Completion>
    {
        auto &loop = self->m_context;
        co_return co_await co_spawn(loop, run(std::move(self), std::move(transfer)), use_awaitable);
    }
    // cofetch must be torn down on its io_context thread, or after that loop has finished.
    static void release(shared_ptr<Transport> self)
    {
        auto &loop = self->m_context;
        if (!loop.stopped()) post(loop, [transport = std::move(self)] {});
    }
    // Aborts the transfers in flight and refuses new ones.
    static void stop(const shared_ptr<Transport> &self)
    {
        post(self->m_context, [weak = weak_ptr<Transport>(self)] {
            if (const auto transport = weak.lock()) {
                transport->m_stopped = true;
                transport->m_stopSignal.cancel();
            }
        });
    }

private:
    static auto run(shared_ptr<Transport> self, Transfer transfer) -> awaitable<Completion>
    {
        using namespace asio::experimental::awaitable_operators;
        if (self->m_stopped) co_return Completion{.outcome = Outcome::refused};
        auto request = Request(std::move(transfer.url));
        request.method(transfer.method)
            .headers(std::move(transfer.headers))
            .body(std::move(transfer.body))
            .timeout(transfer.timeout);
        // Whichever finishes first cancels the other: the response, or stop(). Cancelling this coroutine
        // cancels both, and either may finish first.
        auto outcome = co_await (self->m_client.async_perform(std::move(request), as_tuple(use_awaitable))
            || self->m_stopSignal.async_wait(as_tuple(use_awaitable)));
        if (outcome.index() == 1 || get<0>(get<0>(outcome)) == operation_aborted)
            co_return Completion{.outcome = self->m_stopped ? Outcome::stopped : Outcome::cancelled};
        auto &[error, response] = get<0>(outcome);
        if (error) co_return Completion{.outcome = Outcome::failed, .reason = error.message()};
        co_return Completion{
            .status = static_cast<int>(response.http_code_),
            .headers = std::move(response.header_data_),
            .body = std::move(response.data_),
        };
    }

    io_context &m_context;
    Client m_client;
    // Never expires: stop() cancels the waits to end every transfer in flight.
    steady_timer m_stopSignal;
    bool m_stopped = false;
};
} // namespace network::sdk
