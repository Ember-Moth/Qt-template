module;
#include "taskviewmodel.h"
#include <asio/any_io_executor.hpp>
#include <asio/awaitable.hpp>

export module Template.ViewModels.Task;
import std;
import Template.Tasks;
export import Template.ViewModels.TaskList;

// Match the global-module declaration in the QObject header.
extern "C++" {
struct TaskViewModel::Dependencies
{
    std::shared_ptr<business::TaskService> service;
};
struct TaskViewModel::Initialization
{
    asio::any_io_executor executor;
    asio::awaitable<business::Update> result;
};
}

export using ::TaskViewModel;
