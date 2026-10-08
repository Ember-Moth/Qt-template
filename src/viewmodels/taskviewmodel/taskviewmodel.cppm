module;
#include "runtime/task.h"
#include "taskviewmodel.h"

export module Template.ViewModels.Task;
import std;
import Template.Tasks;
export import Template.ViewModels.TaskList;

using std::shared_ptr;
using std::optional;
using runtime::Task;
using runtime::Executor;
using business::TaskService;
using business::UpdateResult;

// Match the global-module declaration in the QObject header.
extern "C++" {
struct TaskViewModel::Dependencies
{
    shared_ptr<TaskService> service;
    // Where the ViewModel starts service tasks; the service still runs them on its own strand.
    Executor executor;
};
struct TaskViewModel::Initialization
{
    // Empty when the application stopped or failed before the startup result arrived.
    Task<optional<UpdateResult>> result;
};
}

export using ::TaskViewModel;
