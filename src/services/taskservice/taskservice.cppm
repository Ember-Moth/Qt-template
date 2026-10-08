module;
#include "runtime/task.h"

export module Template.Tasks;
import std;
export import Template.Models;
import Template.Storage.Mmkv;

using std::string;
using std::shared_ptr;
using runtime::Task;
using runtime::Executor;
using storage::MmkvStore;

export namespace business {
// The committed tasks after an operation; changed is false when it left them as they were.
struct Update
{
    Tasks tasks;
    bool ready = false;
    bool changed = false;
};
// A failed operation keeps the committed state. Construction failures throw Exception instead.
using UpdateResult = Result<Update>;

// Await the operations from any coroutine: each runs in order on the service's own strand, built
// from the injected executor, and the caller resumes on its own executor.
class TaskService
{
public:
    TaskService(Executor executor, shared_ptr<MmkvStore> store);
    ~TaskService();
    TaskService(const TaskService &) = delete;
    TaskService &operator=(const TaskService &) = delete;

    auto reload() -> Task<UpdateResult>;
    auto addTask(string title) -> Task<UpdateResult>;
    auto setTaskCompleted(string id, bool completed) -> Task<UpdateResult>;
    auto removeTask(string id) -> Task<UpdateResult>;

private:
    struct Impl;
    shared_ptr<Impl> m_impl;
};
} // namespace business
