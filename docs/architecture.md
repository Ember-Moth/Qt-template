# 架构设计

本文解释模板的职责与生命周期；执行规则见 [AGENTS.md](../AGENTS.md)，环境配置和构建命令见 [README.md](../README.md)。

## 分层与依赖

| 位置 | 逻辑模块 | 职责 | Qt 依赖 |
| --- | --- | --- | --- |
| `models/task/` | `Template.Models` | 任务数据、校验与业务错误 | 无 |
| `storage/` | `Template.Storage.Mmkv` | 通用键值读写、存储错误、句柄管理 | 无 |
| `services/taskservice/` | `Template.Tasks` | 任务编解码、状态提交、协程业务接口 | 无 |
| `runtime/` | `Template.Runtime.Asio` | io_context、work guard、后台线程 | 无 |
| `app/asyncmain/` | `Template.App.AsyncMain` | 应用初始化、等待退出、清理编排 | 无 |
| `viewmodels/` | `Template.ViewModels.Task`、`Template.ViewModels.TaskList` | 标准库与 Qt 类型转换、可观察界面状态 | 有 |
| `app/applicationcontext/` | `Template.App.Context` | 装配运行时、存储、服务、生命周期和 ViewModel | 有 |
| `ui/`、`app/main.cpp` | QML 与 Qt 入口 | 页面、类型注册、根组件注入、GUI 事件循环 | 有 |

`template_core` 包含前五行，`template_gui` 包含 ViewModel、ApplicationContext、QML 注册和资源。`app/asyncmain` 自动收集进 core，并从 gui 的 app 文件集合中排除。业务依赖具体 MMKV 与 Asio，不引入 Repository、Runtime 虚接口或后端工厂。

```mermaid
flowchart LR
    UI[QML 页面] --> VM[ViewModel]
    VM --> Service[业务服务]
    Service --> Model[业务模型]
    Service --> Store[通用 MMKV 存储]
    Entry[async_main] --> Service
    Context[ApplicationContext] -.装配与注入.-> VM
    Context -.装配与注入.-> Service
    Context -.装配与注入.-> Store
    Context -.管理生命周期.-> Runtime[AsioRuntime]
    Runtime -.提供执行器.-> Service
    Runtime -.执行根协程.-> Entry
```

Qt 类型止于 ViewModel 适配边界及外侧的 UI、装配和启动代码。业务模型、存储、服务、运行时和 async_main 使用标准库与 Asio 类型。业务结果在 ViewModel 边界排队回 GUI 线程，QObject、Qt 列表模型及可观察状态仅在该线程修改。

## 源码与命名模块

业务、ViewModel 和应用模块按模块建立子目录，接口、实现与可选分区位于同一目录，允许多个实现文件。`storage/{mmkv.cppm,mmkv.cpp}` 与 `runtime/{asioruntime.cppm,asioruntime.cpp}` 直接使用分层目录。

逻辑名称由 `export module` 决定，文件移动不改变 import 名称。CMake 按分层自动发现源码，模块接口加入公开的 `CXX_MODULES` 文件集，由 Ninja 调用 clang-scan-deps 扫描 import 依赖；额外库依赖在 `cmake/Dependencies.cmake` 显式声明。每个模块的 BMI 由声明它的目标编译一次，其他目标共享。Clang 默认生成精简 BMI，只保留模块引用的全局模块片段声明，因此导出模板在导入方实例化时才按 ADL 查找的实体要在模块内显式引用，例如 `asyncmain.cppm` 的 `channelError` 保留 Asio 通道错误码的 `make_error_code` 与 `is_error_code_enum` 特化。

统一使用 C++23，可整体切换 C++26。业务通过 `import std;` 使用标准库，按需列出 `using std::具体类型`。局部变量和工厂结果灵活使用 auto / const auto，公共返回契约、数值宽度与资源所有权保持清晰。Qt、Asio、MMKV 的头文件按 SDK 要求接入。

### QObject 与 Modules

ViewModel 和 ApplicationContext 保留 `.cppm`、`.h`、`.cpp` 混合结构：

- `.h` 声明 QObject、Q_OBJECT、Q_PROPERTY 和信号，供 AUTOMOC 与 `qt_add_qml_module` 的 QML_FOREIGN 注册使用；PIMPL 隔开业务模块类型。
- `.cppm` 在全局模块片段包含头文件，通过 `export using` 暴露类，QObject 类仍属于全局模块。
- 普通实现 `.cpp` 不声明命名模块，导入自己的模块取得业务依赖定义。
- Dependencies、Initialization 在头文件中前置声明，定义位于 `.cppm` 的 `extern "C++"` 块；它们保持全局模块归属，同时引用业务模块类型。
- 模块不导出宏。使用 Qt 宏的消费者包含对应 Qt 头文件，注册适配代码直接包含 QObject 头文件。

Task 模块重导出 TaskList 模块。应用与测试使用 `import Template.ViewModels.Task;`。这种组织保留 Qt 的生成与链接流程。[Qt moc](https://doc.qt.io/qt-6.12/moc.html)、[模块归属规则](https://eel.is/c++draft/module.unit#7.2.4)。

## 启动、初始化与退出

### 正常启动

1. main 创建 QGuiApplication，设置应用身份与样式。
2. ApplicationContext 装配 AsioRuntime、MmkvStore、TaskService、Lifecycle 和 TaskViewModel；构造不启动业务加载。
3. QML 引擎接收 appContext，加载根窗口；页面通过 required property 接收具体 ViewModel。
4. main 调用 `context.start()`，再进入 `app.exec()`。start 只接受一次，重复或停止后调用返回 false。
5. ApplicationContext 依次调用已接线 ViewModel 的 initialize()，再启动根协程。TaskViewModel.initialize() 设置 busy，接收自己的 `Startup<Update>` 结果 awaitable。
6. async_main 按顺序执行启动步骤：每一步在服务的 executor() 上 co_spawn / co_await 加载操作并交付结果；Qt 完成处理器更新列表、ready、busy 和错误信息。根协程随后等待退出通知。

每个启动结果是一个容量为 1 的 Asio concurrent_channel，由 `lifecycle->startup<Result>()` 登记，只交付一次、只有一个消费者；Lifecycle 另用一个通道保存退出通知。`Dependencies` 只含 Lifecycle 与有序的启动步骤，`startup_step(service, &Service::operation, startup)` 把服务操作与其结果绑定，因此 async_main 不认识具体服务，新增功能无需修改 `app/asyncmain`。请求退出时尚未开始的步骤被跳过，所有登记的结果被取消，消费者收到空结果（`nullopt`）而非异常；退出后才登记的结果立即取消。

初始化的存储错误作为业务结果交给界面，页面可调用 reload() 重试，根协程继续等待退出。未处理的根协程异常取消所有启动结果，已交付的结果不受影响，并在 Qt 边界记录错误并请求应用退出。独立构造 ViewModel 时由调用方显式 reload()，应用级首次加载由 async_main 发起。

### 退出与成员销毁

QCoreApplication 的 aboutToQuit 连接到 context.stop()。stop 幂等：先禁止新的界面命令，再向 Lifecycle 请求停止；退出早于根协程执行时，初始化被跳过；退出发生在初始化期间时，启动结果等待者被释放，有限的业务操作继续完成。停止后的 Qt 完成处理器忽略界面更新。

外层对象按 QGuiApplication、ApplicationContext、QQmlApplicationEngine 的顺序声明，QML 引擎先析构。ApplicationContext 内部依次持有 AsioRuntime、MMKV 存储、服务、Lifecycle、ViewModel。Impl 析构函数先再次请求停止，再调用运行时 finish()，此时 ViewModel 和服务仍存在；根协程与已接受操作结束、线程 join 后，成员再按相反顺序销毁，Qt application 最后销毁。

因此清理不依赖 GUI 线程处理任何回复，Qt 事件循环结束后也能完成。示例资源由 RAII 释放。新增持续任务、定时器或网络初始化时，所属业务须响应退出请求，取消持续 I/O，并在 async_main 返回前 await 清理完成。

## 错误处理

可恢复的失败沿 `std::expected` 返回：MmkvStore 返回 `storage::Result<T>`，TaskService 的协程返回 `asio::awaitable<UpdateResult>`（`expected<Update, business::Error>`），失败时已提交状态不变。ViewModel 收到错误后翻译为界面文案；加载失败另将 ready 置为 false。

错误码是各层的 `enum class ErrorCode`，`Error::detail` 由外向内带上下文，例如 `add task: write 'tasks.items' to MMKV store 'app' in '/…/mmkv': the store is open read-only`。存储层给出实例、目录与键，服务层加上操作与任务 ID，模型层给出字段、记录序号与偏移。

异常只用于构造失败与违反使用约定：`business::Exception`、`application::Exception` 携带同样的错误码与上下文，例如缺少服务或执行器、在其他执行器上运行 TaskService 协程、重复初始化 ViewModel、重复运行 async_main。MMKV SDK 自身的异常在存储边界转为 `storage::Error`；不可恢复的异常在 ViewModel 显示为操作失败，在 ApplicationContext 记录后退出应用。

## 执行器与业务状态

AsioRuntime 是具体资源所有者，管理一个 io_context、work guard 和一条后台线程。服务接收应用注入的 any_io_executor，各自用 make_strand 建立执行器，不创建自己的事件循环或线程。

运行时执行器用于构造服务，服务执行器用于启动其业务协程；直接把服务 awaitable 放在其他执行器上运行会违反契约。跨服务编排通过 `co_await co_spawn(service.executor(), operation, use_awaitable)` 切换到目标服务执行器。业务接口返回 awaitable，不接收完成回调；Qt 和测试边界可以使用完成处理器与 use_future。

业务状态仅在所属服务执行器上访问，MMKV 的同步调用留在后台线程。已创建的协程捕获共享状态，即使服务或 ViewModel 句柄先销毁仍可完成。公共运行时必须活过使用者，停止新提交后由所属线程 finish() 排空操作并 join。[Asio io_context](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/io_context.html)。

ViewModel 的 Q_INVOKABLE 返回 true 只表示命令已接受。业务先保存候选数据，再提交内存状态；Qt 边界随后更新界面，新增成功才清空输入。保存失败保留已有业务状态、复选框状态和输入内容。

不同任务的命令可以同时进行：服务 strand 依次执行，每个结果都是完整快照，按完成顺序应用到界面。ViewModel 只锁定受影响的部分：保存中的任务在列表模型中标记为 pending，只禁用该行；新增进行中时 adding 锁定输入，保证成功后清空的是已提交的文字；加载替换整个列表，loading 期间拒绝其他命令，加载也要等已接受的命令完成后才开始。busy 表示还有操作进行，只用于进度提示。被拒绝的命令返回 false 并发出 commandRejected(reason)，页面显示短暂提示。

错误按加载、新增、任务 ID 分别保留，errorMessage 汇总尚未解决的错误。成功只清除该目标的错误：新增重试成功清除新增错误，某行更新或删除成功清除该行错误，加载成功清除加载错误。其他任务的成功不会隐藏失败；多个目标失败时，可以分别重试并逐项清除。任务一旦不在新应用的快照中（例如对已不存在的任务报出 notFound），就无法再针对它取得成功，它的错误在下一次列表更新时清除。

## 通用 MMKV 后端

MmkvStore 不导入业务模型，公共 API 使用标准库类型与独立的 storage::Error。SDK 头文件集中在 mmkv_sdk.cppm 的全局模块片段；该文件声明私有实现分区 Template.Storage.Mmkv:sdk，存储实现通过 import :sdk 使用其声明。分区不会从主接口导出，SDK 的宏与 TU-local 操作留在分区内部，保持原生 C++ 链接、SDK 类型的全局模块归属与 include-before-import 顺序，也避免 clangd 合并 SDK 头文件与标准库模块时出现类型歧义。Ninja 的模块依赖扫描同时跟踪实现分区，修改它时会使导入它的模块单元重新编译。

后端负责键值读写、文件完整性、操作锁、同步与句柄生命周期。打开前完整校验 CRC，避免 MMKV 默认恢复丢弃数据；之后的访问比较数据与元数据文件的大小和修改时间，发现外部改动时重新完整校验。TaskService 负责 tasks.items 键、记录编解码、业务校验与错误转换。任务示例使用原生字符串列表，每条记录按 ID、标题、完成标记排列。

getter 返回 expected<optional<T>, storage::Error>，区分缺失键、空值与读写错误。MMKV 没有逐键类型标签，调用方使用匹配的 setter/getter；业务之间通过具名键或独立实例隔离。相同目录与实例 ID 共享句柄和操作锁，最后一个引用释放时关闭。

写入成功按 SDK set 的返回值判断，sync 返回 void，模板不把它视为可检测的断电持久性确认。存储与业务测试使用真实 MMKV 临时实例，不引入可替换存储接口。模板不包含历史格式兼容、JSON 依赖或数据迁移；多进程、加密配置和运维策略由使用模板的应用按需求接入。

## 扩展业务与 ViewModel

1. 在 models / services 下添加业务模块与协程接口，使用应用注入的执行器；存储通过具名键或独立 MMKV 实例使用。源码由 CMake 自动发现。
2. 添加 ViewModel 模块目录、QObject 头文件、导入入口与实现；Dependencies 只列出所需服务。需要应用级初始化时定义 `Initialization{executor, result}` 与 `initialize()`、`stop()`，结果类型与服务的启动操作一致。
3. 在 ApplicationContext::Impl 依次声明服务与 ViewModel 成员（context 为应用级 ViewModel 的 QObject 父对象），并在构造函数中调用一次 `attach(viewModel, service, &Service::operation)`；启动步骤、初始化、退出与析构顺序由它统一处理，`app/asyncmain` 无需修改。不需要启动加载的 ViewModel 调用 `attach(viewModel)`，只登记退出时的 `stop()`。
4. 给 ApplicationContext 增加强类型只读 Q_PROPERTY，在 ui/qmltypes.h 添加 QML_FOREIGN / QML_NAMED_ELEMENT / QML_UNCREATABLE 注册；main 保持统一的 appContext 注入。
5. 根组件将具体 ViewModel 传给页面，页面声明 required property；新增 QML 文件由 `qt_add_qml_module` 自动收集进资源、qmldir、qmllint 与 qmlcachegen；文件平铺在 `/qt/qml/Template/Ui` 下，因此 QML 文件名在 src/ui 内唯一。`Template.Ui` 构建为静态插件，应用与 QML 测试通过 `Q_IMPORT_QML_PLUGIN(Template_UiPlugin)` 导入。
6. 按改动验证 C++23/26、受影响的业务与 ViewModel/QML 测试、qmllint、clangd；涉及业务边界时检查无 Qt 构建。验证要求见 [AGENTS.md](../AGENTS.md#验证与编辑器)。

同一份界面状态复用同一个 ViewModel；页面需要独立状态时，由对应作用域创建、持有并注入共享服务。类型注册与实例装配分开，QML 不直接创建应用持有的实例。[Qt 注册宏](https://doc.qt.io/qt-6/qqmlintegration-h.html#QML_UNCREATABLE)。
