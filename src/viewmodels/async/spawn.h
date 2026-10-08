#pragma once

// The one way the Qt side starts asynchronous work. A header rather than a module: it is included only
// by Qt units, and clangd misreads Qt and Asio headers mixed with import std inside a module unit.
// Names stay qualified so nothing leaks into the units that include it.
#include "runtime/task.h"
#include <asio/co_spawn.hpp>
#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QPointer>

namespace viewmodels {
// Runs task on executor, then calls onResult on the GUI thread, unless receiver has been destroyed by
// then: onResult(failure, value) like an Asio completion, or onResult(failure) for Task<void>. failure
// holds the exception that ended the task, and value is default-constructed in that case. Call it on
// the GUI thread.
template <class T, class OnResult>
void spawn(runtime::Executor executor, runtime::Task<T> task, QObject *receiver, OnResult onResult)
{
    // The QPointer is read only on the GUI thread; the application outlives every runtime task.
    auto guard = QPointer<QObject>(receiver);
    auto *application = QCoreApplication::instance();
    asio::co_spawn(std::move(executor), std::move(task), [guard, application, onResult](auto &&...result) {
        QMetaObject::invokeMethod(application, [guard, onResult, ... result = std::move(result)]() mutable {
            if (guard) onResult(std::move(result)...);
        }, Qt::QueuedConnection);
    });
}
} // namespace viewmodels
