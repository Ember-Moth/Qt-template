#pragma once

// The coroutine vocabulary, as a header included into each unit's global module fragment, the way
// every unit already includes the Asio headers it uses. Importing it through modules chains Asio
// declarations across interfaces, which clangd 23 cannot merge.
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

namespace runtime {
// An asynchronous operation: declare a function returning Task<T>, co_await other tasks inside it and
// co_return a T. Awaiting a task never needs an executor; a service moves onto its own executor itself.
template <class T = void> using Task = asio::awaitable<T>;
// Where coroutines run. Only infrastructure handles one: the runtime, services building their strands,
// and the Qt boundary that starts tasks.
using Executor = asio::any_io_executor;
} // namespace runtime
