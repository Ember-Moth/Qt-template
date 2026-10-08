module;
#include "runtime/task.h"

export module Template.Tasks.Import;
import std;
export import Template.Models;
import Template.Network.Http;

using std::string;
using std::shared_ptr;
using runtime::Task;
using runtime::Executor;
using network::HttpClient;

export namespace business {
// Tasks read from a remote source, with new local ids; nothing is saved.
using ImportResult = Result<Tasks>;

// Reads tasks from a JSON endpoint shaped like [{"title": "...", "completed": false}, ...], ignoring
// other fields. Await it from any coroutine; construction failures throw Exception.
class TaskImportService
{
public:
    TaskImportService(Executor executor, shared_ptr<HttpClient> http);
    ~TaskImportService();
    TaskImportService(const TaskImportService &) = delete;
    TaskImportService &operator=(const TaskImportService &) = delete;

    auto fetchTasks(string url) -> Task<ImportResult>;

private:
    struct Impl;
    shared_ptr<Impl> m_impl;
};
} // namespace business
