# 项目开发约定

本仓库是 Windows、macOS、Linux 的 Qt QML + MVVM 开发模板。后续修改遵循以下约定；详细设计见 [docs/architecture.md](docs/architecture.md)，构建命令见 [README.md](README.md)。

README 提供上手、配置与验证说明，架构文档解释职责与生命周期，本文记录执行规则。目录、模块入口、初始化或退出契约变化时，同步相关文档；本机验证与实际运行的远端 CI 分别说明。

## 分层与目录

- 源码放在 `src/`，按 `models`、`storage`、`services`、`runtime`、`viewmodels`、`ui`、`app` 分层。
- QML 页面、组件和主题放在 `src/ui/`，统一使用 `ui` 命名。
- QML 资源生成器按文件名收集类型，`src/ui/` 内的 QML 文件名保持唯一。
- 业务与 ViewModel 模块按模块建立子目录，目录名与主接口文件名对应，例如 `models/task/{task.cppm,task.cpp}`。接口、实现及模块分区放在同一目录，允许多个实现文件。
- `storage` 固定使用 MMKV，`runtime` 固定使用 Asio，各自直接作为模块目录：`storage/{mmkv.cppm,mmkv.cpp}`、`runtime/{asioruntime.cppm,asioruntime.cpp}`，不再嵌套后端子目录。
- 模块的逻辑名称由 `export module` 声明决定。构建统一使用 xmake，按分层自动发现 `.cppm` 和 `.cpp`，Qt 层同时发现 `.h`；现有分层中新增模块无需修改构建文件，额外的库依赖仍需显式声明。

## Qt 边界与 ViewModel Modules

- Qt 依赖止于 ViewModel 适配边界及其外侧的 UI、装配与启动代码。`models`、`storage`、`services`、`runtime` 和 `app/asyncmain` 使用标准 C++ 类型、Asio 和 MMKV，不引入 Qt 类型或 Qt 事件循环。
- Qt 列表适配器属于 `viewmodels`；业务模型保持独立于 Qt，可在无 Qt 配置下构建和测试。
- ViewModel 使用 `.cppm`、`.h`、`.cpp` 的混合结构，三个文件放在同一个模块目录。
- `.h` 声明 QObject、Q_OBJECT、Q_PROPERTY 和信号，供 xmake 的 Qt moc 规则与 QML_FOREIGN 注册工具处理；通过 PIMPL 隔开业务模块类型。
- `.cppm` 在全局模块片段包含对应头文件，通过 `export using` 导出类。QObject 类仍属于全局模块，普通实现 `.cpp` 不添加命名模块声明。
- 应用和测试通过 `import Template.ViewModels.Task;` 等模块入口使用 ViewModel。Qt 注册适配文件继续包含 QObject 头文件；使用 Qt 宏的消费者包含对应 Qt 头文件，模块不导出宏。
- 类型注册与实例装配分开：`ui/qmltypes.h` 使用 QML_FOREIGN、QML_NAMED_ELEMENT、QML_UNCREATABLE 注册类型；`app/applicationcontext/` 统一创建运行时、MMKV 存储、服务和 ViewModel。
- ViewModel 接收显式 `Dependencies` 构造参数，只包含所需服务，不自行创建运行时、MMKV 存储或业务服务。QObject 头文件只前置声明 Dependencies 和 Initialization；其定义放在 `.cppm` 的 `extern "C++"` 块中，以保持全局模块归属并引用业务模块类型。
- ApplicationContext 通过具名、强类型的只读 Q_PROPERTY 暴露 ViewModel；应用入口只向 QML 根组件注入 appContext。根组件向子页面传递具体 ViewModel，页面使用 required property 声明依赖。
- 新增 ViewModel 时添加其模块文件、QML 类型注册及 ApplicationContext 装配与属性；应用启动入口的注入形式保持统一。应用级 ViewModel 由 ApplicationContext 持有，共享服务显式注入；页面实例需要独立状态时由对应作用域创建和持有。
- QObject、Qt 列表模型及可观察界面状态只在 GUI 线程修改；业务结果在 ViewModel 边界排队回 GUI 线程。

## C++ 写法与异步接口

- 项目默认统一使用 C++23，保留 C++26 配置选项，使用同一套兼容的编译器与标准库工具链。三个平台统一使用 Clang，标准库与该平台的 Qt SDK 一致：macOS 用 libc++，Linux 用 libstdc++，Windows 用 MSVC STL；不使用 GCC 或 MSVC 编译器。
- 业务代码使用命名模块及标准 `import std;`，按需使用 `using std::具体类型`，不使用 `using namespace std`。不改回标准库头文件方案，也不假设工具链提供可供外部导入的 `std:vector` 等逐类型分区。Asio、MMKV、Qt 的头文件仍按其工具链要求使用，并放在 `import std;` 之前（模块单元放在全局模块片段）；MSVC STL 不支持先 import 后 include。
- 灵活使用 `auto`、`const auto`、引用和有意义的类型别名，避免重复长类型名；公共接口的返回契约、数值宽度和资源所有权保持清晰。
- Asio 业务接口使用 `asio::awaitable`、`co_await` 和 `co_return`，调用方在服务执行器上用 `co_spawn` 启动。业务接口不接收完成回调；Qt 桥接和测试边界可以使用完成处理器或 `use_future`。
- Asio 公共运行时放在 `runtime`，由 ApplicationContext 创建并统一管理其生命周期。运行时管理 `io_context`、work guard 与后台线程；业务服务接收注入的执行器，不创建自己的事件循环或线程，不依赖其他业务服务获取公共运行时。
- 服务基于注入的执行器创建自己的 strand，业务协程在 `service.executor()` 上启动，业务状态只在该执行器上访问。MMKV 的同步操作放在运行时后台线程执行。
- 已创建的业务协程持有共享状态，服务对象销毁后仍可完成。公共运行时必须活过所有使用者；停止提交后在应用所属线程调用 `finish()` 或析构，等待已提交操作完成。长期 I/O 需由所属业务先取消。
- 应用级异步入口为 `app/asyncmain/` 的 `Template.App.AsyncMain`，使用 `async_main(Dependencies) -> asio::awaitable<void>`；模块不依赖 Qt，归入 template_core，可在无 Qt 配置下验证。AsioRuntime 管理运行时资源，ApplicationContext 装配依赖，async_main 按顺序执行 Dependencies 中的启动步骤并编排退出清理；async_main 不依赖具体业务服务，新增功能不修改该模块。
- ApplicationContext 构造时只装配，main 加载 QML 后调用一次 start()，再运行 Qt app.exec()。首次业务加载由 async_main 发起；ViewModel 构造时不自动 reload，通过 Initialization 接收启动结果，独立使用时显式调用 reload()。
- Lifecycle 的启动结果与退出通知使用 Asio 通道，不给业务接口添加完成回调；每个需要启动加载的功能通过 `lifecycle->startup<Result>()` 登记一个只有单一消费者的 `Startup<Result>`，由 `startup_step` 在服务执行器上运行加载并交付，退出或启动失败时统一取消。ApplicationContext 用 `attach(viewModel, service, &Service::operation)` 为功能接线启动步骤、`initialize()` 与 `stop()`；没有启动加载的 ViewModel 用 `attach(viewModel)`，只在退出时接收 `stop()`。退出通知可早于协程等待；stop() 幂等并拒绝新的界面命令，aboutToQuit 与析构都通知退出。Impl 析构先请求退出，在 ViewModel 和服务仍存在时 finish() 等待后台操作与线程结束，再按成员顺序释放资源；退出清理不等待 GUI 回调。新增持续 I/O 时由所属业务响应退出、取消操作，并在 async_main 返回前等待清理完成。

## 错误处理

- 可恢复的失败通过 `std::expected<T, Error>` 返回，各层使用自己的 `enum class ErrorCode` 与 `Error`（`business`、`storage`、`application`）。业务失败、校验失败、I/O 与格式错误都不抛异常。
- `Error::detail` 按 `<上下文>: <原因>` 书写，由外向内列出操作、对象（任务 ID、键、MMKV 实例与目录、记录序号）和原因；向上传递时用 `withContext` 或同一格式加上外层上下文，不丢弃内层信息。
- 只在构造失败（缺少依赖）和调用方违反使用约定（在错误的执行器上运行、重复初始化或重复启动）时抛出所在层的 `Exception`，它携带同样的 `ErrorCode` 与带上下文的 `Error`；不抛标准库异常。退出导致的启动结果取消是正常流程，返回空结果。
- 第三方库的异常在边界转换：MMKV SDK 抛出的异常在 storage 内转为 `storage::Error`，只有 `std::bad_alloc` 继续传播；业务服务不捕获异常，到达 ViewModel 与 ApplicationContext 的异常按不可恢复处理，在 Qt 边界显示或记录。

## 模板范围与持久化

- 固定使用 MMKV 和 Asio，直接依赖具体实现；不为假设的后端替换引入 Repository、Runtime 抽象接口、工厂或适配层。
- MMKV 代码集中在 `storage/`，由通用 MmkvStore 负责键值读写与句柄管理；任务键、任务记录格式和业务校验由 TaskService 负责；AsioRuntime 位于 `runtime/`，保留集中管理运行时资源的职责。依赖注入用于装配和生命周期管理。
- 存储与业务测试使用临时目录中的真实 MMKV，不为测试新增可替换存储接口。
- 持久化后端使用 MMKV 官方 C++ Core，SDK 类型集中在 `storage/mmkv.cpp` 的实现；公共 API 只使用标准库类型与独立的 storage::Error，不导入任务或其他业务模型。
- ApplicationContext 创建共享 MmkvStore 并注入服务；不同业务使用具名键（如 settings.theme、tasks.items），按需使用独立的实例 ID。任务示例的编解码在 TaskService 中完成。
- 保持模板简洁，不恢复 JSON 依赖、旧格式兼容或数据迁移代码，不恢复独立的 `business_main.cpp` 示例入口。
- 持久化成功后提交业务状态，失败保留已提交状态；新增成功后才清空界面输入。
- 保存期间只锁定受影响的界面：不同任务的命令可以并发，服务 strand 按顺序执行并返回完整快照；保存中的任务、进行中的新增与加载各自拒绝新命令。被拒绝的命令返回 false 并发出带原因的 `commandRejected`，界面据此提示，不整体禁用。

## 验证与编辑器

- 与 Modules、QObject 或 QML 注册有关的改动，验证受影响的 C++23/C++26 构建、ViewModel/QML 测试及 qmllint；涉及业务边界时检查无 Qt 构建。
- 保持 Zed 的 clangd Modules 配置；xmake 构建自动导出根目录 `compile_commands.json`，包含标准库和命名模块的编译命令。目录或模块入口变化后确认编译数据库与 clangd 可识别新路径。
- 不提交 BMI、构建产物或包含本机路径的编译数据库。验证结果区分本机检查与实际运行过的远端 CI。

<!-- astrlink-debug:begin -->
## AstrLink local data

AstrLink added this section with its agent debugging tools and removes it when they are uninstalled.

- Inspect AstrLink only through the read-only CLI that the `astrlink-debug` skill describes.
- Do not read, copy, search, or open AstrLink's data directory (`/Users/tschen/Library/Application Support/com.astrlink.desktop`), any `astrlink.db*` file, or `~/.astrlink/control-session.json`, and do not run `sqlite3` on them.
- The control socket and the session token only carry observer access. Do not use them to change AstrLink settings.
<!-- astrlink-debug:end -->
