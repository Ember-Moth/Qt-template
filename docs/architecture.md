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

逻辑名称由 `export module` 决定，文件移动不改变 import 名称。xmake 自动发现既有分层的源码，模块接口设为 public，由工具链扫描 import 依赖；额外库依赖显式声明。

统一使用 C++23，可整体切换 C++26。业务通过 `import std;` 使用标准库，按需列出 `using std::具体类型`。局部变量和工厂结果灵活使用 auto / const auto，公共返回契约、数值宽度与资源所有权保持清晰。Qt、Asio、MMKV 的头文件按 SDK 要求接入。

### QObject 与 Modules

ViewModel 和 ApplicationContext 保留 `.cppm`、`.h`、`.cpp` 混合结构：

- `.h` 声明 QObject、Q_OBJECT、Q_PROPERTY 和信号，供 moc 与 QML_FOREIGN 使用；PIMPL 隔开业务模块类型。
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
5. ApplicationContext 启动启动结果消费者与根协程。TaskViewModel.initialize() 设置 busy，接收 Lifecycle.startup() 的结果 awaitable。
6. async_main 在 TaskService.executor() 上 co_spawn / co_await reload，并交付 Update；Qt 完成处理器更新列表、ready、busy 和错误信息。根协程随后等待退出通知。

Lifecycle 使用容量为 1 的 Asio concurrent_channel，分别交付一次启动结果和保存退出通知。startup() 是单消费者接口；当前结果是待办服务的 Update。扩展多服务初始化时，按需求扩展启动结果与显式 Dependencies，并在 Qt 装配边界分发给对应 ViewModel。

初始化的存储错误作为业务结果交给界面，页面可调用 reload() 重试，根协程继续等待退出。未处理的根协程异常关闭启动通道，在 Qt 边界记录错误并请求应用退出。独立构造 ViewModel 时由调用方显式 reload()，应用级首次加载由 async_main 发起。

### 退出与成员销毁

QCoreApplication 的 aboutToQuit 连接到 context.stop()。stop 幂等：先禁止新的界面命令，再向 Lifecycle 请求停止；退出早于根协程执行时，初始化被跳过；退出发生在初始化期间时，启动结果等待者被释放，有限的业务操作继续完成。停止后的 Qt 完成处理器忽略界面更新。

外层对象按 QGuiApplication、ApplicationContext、QQmlApplicationEngine 的顺序声明，QML 引擎先析构。ApplicationContext 内部依次持有 AsioRuntime、MMKV 存储、服务、Lifecycle、ViewModel。Impl 析构函数先再次请求停止，再调用运行时 finish()，此时 ViewModel 和服务仍存在；根协程与已接受操作结束、线程 join 后，成员再按相反顺序销毁，Qt application 最后销毁。

因此清理不依赖 GUI 线程处理任何回复，Qt 事件循环结束后也能完成。示例资源由 RAII 释放。新增持续任务、定时器或网络初始化时，所属业务须响应退出请求，取消持续 I/O，并在 async_main 返回前 await 清理完成。

## 执行器与业务状态

AsioRuntime 是具体资源所有者，管理一个 io_context、work guard 和一条后台线程。服务接收应用注入的 any_io_executor，各自用 make_strand 建立执行器，不创建自己的事件循环或线程。

运行时执行器用于构造服务，服务执行器用于启动其业务协程；直接把服务 awaitable 放在其他执行器上运行会违反契约。跨服务编排通过 `co_await co_spawn(service.executor(), operation, use_awaitable)` 切换到目标服务执行器。业务接口返回 awaitable，不接收完成回调；Qt 和测试边界可以使用完成处理器与 use_future。

业务状态仅在所属服务执行器上访问，MMKV 的同步调用留在后台线程。已创建的协程捕获共享状态，即使服务或 ViewModel 句柄先销毁仍可完成。公共运行时必须活过使用者，停止新提交后由所属线程 finish() 排空操作并 join。[Asio io_context](https://think-async.com/Asio/asio-1.38.2/doc/asio/reference/io_context.html)。

ViewModel 的 Q_INVOKABLE 返回 true 只表示命令已接受。业务先保存候选数据，再提交内存状态；Qt 边界随后更新界面，新增成功才清空输入。保存失败保留已有业务状态、复选框状态和输入内容。

## 通用 MMKV 后端

MmkvStore 不导入业务模型，公共 API 使用标准库类型与独立的 storage::Error。SDK 类型集中在存储实现中：SDK 头文件放在实现单元的全局模块片段，先于 import std 包含，保持原生 C++ 链接和全局模块归属，也符合 MSVC STL 只支持先 include 后 import 的要求。

后端负责键值读写、文件完整性、操作锁、同步与句柄生命周期。打开前完整校验 CRC，避免 MMKV 默认恢复丢弃数据；之后的访问比较数据与元数据文件的大小和修改时间，发现外部改动时重新完整校验。TaskService 负责 tasks.items 键、记录编解码、业务校验与错误转换。任务示例使用原生字符串列表，每条记录按 ID、标题、完成标记排列。

getter 返回 expected<optional<T>, storage::Error>，区分缺失键、空值与读写错误。MMKV 没有逐键类型标签，调用方使用匹配的 setter/getter；业务之间通过具名键或独立实例隔离。相同目录与实例 ID 共享句柄和操作锁，最后一个引用释放时关闭。

写入成功按 SDK set 的返回值判断，sync 返回 void，模板不把它视为可检测的断电持久性确认。存储与业务测试使用真实 MMKV 临时实例，不引入可替换存储接口。模板不包含历史格式兼容、JSON 依赖或数据迁移；多进程、加密配置和运维策略由使用模板的应用按需求接入。

## 扩展业务与 ViewModel

1. 在 models / services 下添加业务模块与协程接口，使用应用注入的执行器；存储通过具名键或独立 MMKV 实例使用。源码由 xmake 自动发现。
2. 添加 ViewModel 模块目录、QObject 头文件、导入入口与实现；Dependencies 只列出所需服务。需要应用级初始化时定义 Initialization，并在 async_main 中安排服务初始化。
3. 在 ApplicationContext 装配服务、Lifecycle 与 ViewModel，分发启动结果，维护 stop 与析构顺序，为应用级 ViewModel 设置 context 为 QObject 父对象。
4. 给 ApplicationContext 增加强类型只读 Q_PROPERTY，在 ui/qmltypes.h 添加 QML_FOREIGN / QML_NAMED_ELEMENT / QML_UNCREATABLE 注册；main 保持统一的 appContext 注入。
5. 根组件将具体 ViewModel 传给页面，页面声明 required property；新增 QML 文件自动进入资源与 qmldir，当前资源生成器要求 QML 文件名在 src/ui 内唯一。
6. 按改动验证 C++23/26、受影响的业务与 ViewModel/QML 测试、qmllint、clangd；涉及业务边界时检查无 Qt 构建。验证要求见 [AGENTS.md](../AGENTS.md#验证与编辑器)。

同一份界面状态复用同一个 ViewModel；页面需要独立状态时，由对应作用域创建、持有并注入共享服务。类型注册与实例装配分开，QML 不直接创建应用持有的实例。[Qt 注册宏](https://doc.qt.io/qt-6/qqmlintegration-h.html#QML_UNCREATABLE)。
