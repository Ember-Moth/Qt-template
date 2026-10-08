# 项目开发约定

本仓库是 Windows、macOS、Linux 的 Qt QML + MVVM 开发模板。后续修改遵循以下约定；详细设计见 [docs/architecture.md](docs/architecture.md)，构建命令见 [README.md](README.md)。

README 提供上手、配置与验证说明，架构文档解释职责与生命周期，本文记录执行规则。目录、模块入口、初始化或退出契约变化时，同步相关文档；本机验证与实际运行的远端 CI 分别说明。

## 分层与目录

- 源码放在 `src/`，按 `errors`、`models`、`storage`、`network`、`services`、`runtime`、`viewmodels`、`ui`、`app` 分层；`errors` 是各层共用的错误处理模块，自身不依赖其他层。
- QML 页面、组件和主题放在 `src/ui/`，统一使用 `ui` 命名。
- `qt_add_qml_module` 以文件名作为 QML 类型名，并把文件平铺在 `/qt/qml/Template/Ui` 下，`src/ui/` 内的 QML 文件名保持唯一；单例 QML 以 `pragma Singleton` 标记，构建自动识别；QML 文件是配置依赖，修改后构建会重新配置以更新 qmldir。
- 业务与 ViewModel 模块按模块建立子目录，目录名与主接口文件名对应，例如 `models/task/{task.cppm,task.cpp}`。接口、实现及模块分区放在同一目录，允许多个实现文件。
- `storage` 固定使用 MMKV，`network` 固定使用 cofetch（基于 libcurl），`runtime` 固定使用 Asio，各自直接作为模块目录：`storage/{mmkv.cppm,mmkv.cpp}`、`network/{httpclient.cppm,httpclient.cpp}`、`runtime/{asioruntime.cppm,asioruntime.cpp}`，不再嵌套后端子目录。
- 模块的逻辑名称由 `export module` 声明决定。构建统一使用 CMake 4.4.x、Ninja 与 `CMakePresets.json`，按分层用 `CONFIGURE_DEPENDS` 自动发现 `.cppm`（模块文件集）和 `.cpp`，Qt 层同时收集 `.h`；现有分层中新增模块无需修改构建文件，额外的库依赖仍需在 `cmake/Dependencies.cmake` 中显式声明并固定 SHA-256。
- `import std` 依赖 CMake 4.4 的实验开关，UUID 位于 `cmake/ImportStd.cmake` 且只对应 CMake 4.4.x；升级 CMake 时同步更新 UUID 与 CI 中固定的 CMake 版本。用 `qt_add_executable` 创建的目标需调用 `template_scan_modules` 开启模块扫描。
- libcurl 在 macOS 与 Linux 使用系统库（`find_package(CURL)`，Linux 需 libcurl 开发包），在 Windows 由 `cmake/Dependencies.cmake` 以固定 SHA-256 的源码静态构建并使用 Schannel；三处都提供 `CURL::libcurl`。cofetch 与 glaze 同样固定版本与 SHA-256。
- 发布包由 `install()` 与 `qt_generate_deploy_qml_app_script()` 生成，`cmake --install` 调用 macdeployqt、windeployqt 或 Qt 的 Linux 部署逻辑；不手写 Qt 库、插件或 QML 模块的复制规则。

## Qt 边界与 ViewModel Modules

- Qt 依赖止于 ViewModel 适配边界及其外侧的 UI、装配与启动代码。`errors`、`models`、`storage`、`network`、`services`、`runtime` 和 `app/asyncmain` 使用标准 C++ 类型、Asio、MMKV、cofetch 与 glaze，不引入 Qt 类型或 Qt 事件循环。
- Qt 列表适配器属于 `viewmodels`；业务模型保持独立于 Qt，可在无 Qt 配置下构建和测试。
- ViewModel 使用 `.cppm`、`.h`、`.cpp` 的混合结构，三个文件放在同一个模块目录。
- `.h` 声明 QObject、Q_OBJECT、Q_PROPERTY 和信号，供 AUTOMOC 与 `qt_add_qml_module` 的 QML_FOREIGN 类型注册处理；通过 PIMPL 隔开业务模块类型。
- `.cppm` 在全局模块片段包含对应头文件，通过 `export using` 导出类。QObject 类仍属于全局模块，普通实现 `.cpp` 不添加命名模块声明。
- 应用和测试通过 `import Template.ViewModels.Task;` 等模块入口使用 ViewModel。Qt 注册适配文件继续包含 QObject 头文件；使用 Qt 宏的消费者包含对应 Qt 头文件，模块不导出宏。
- 类型注册与实例装配分开：`ui/qmltypes.h` 使用 QML_FOREIGN、QML_NAMED_ELEMENT、QML_UNCREATABLE 注册类型；`app/applicationcontext/` 统一创建运行时、MMKV 存储、服务和 ViewModel。
- ViewModel 接收显式 `Dependencies` 构造参数，只包含所需服务和启动任务所用的 `runtime::Executor`，不自行创建运行时、MMKV 存储或业务服务。QObject 头文件只前置声明 Dependencies 和 Initialization；其定义放在 `.cppm` 的 `extern "C++"` 块中，以保持全局模块归属并引用业务模块类型。
- ApplicationContext 通过具名、强类型的只读 Q_PROPERTY 暴露 ViewModel；应用入口只向 QML 根组件注入 appContext。根组件向子页面传递具体 ViewModel，页面使用 required property 声明依赖。
- 新增 ViewModel 时添加其模块文件、QML 类型注册及 ApplicationContext 装配与属性；应用启动入口的注入形式保持统一。应用级 ViewModel 由 ApplicationContext 持有，共享服务显式注入；页面实例需要独立状态时由对应作用域创建和持有。
- QObject、Qt 列表模型及可观察界面状态只在 GUI 线程修改。Qt 侧统一用 `viewmodels/async/spawn.h` 的 `viewmodels::spawn(executor, task, receiver, onResult)` 启动任务：它在运行时上执行任务，再按 Asio 完成处理器的形式 `onResult(exception_ptr failure, T value)` 把结果排队回 GUI 线程，接收对象已销毁时跳过；ViewModel 与 ApplicationContext 不直接调用 `co_spawn` 或跨线程投递。

## C++ 写法与异步接口

- 项目默认统一使用 C++23，保留 C++26 配置选项，使用同一套兼容的编译器与标准库工具链。三个平台统一使用 Clang，标准库与该平台的 Qt SDK 一致：macOS 用 libc++，Linux 用 libstdc++，Windows 用 MSVC STL；不使用 GCC 或 MSVC 编译器。
- 业务代码使用命名模块及标准 `import std;`，不使用 `using namespace std`。灵活使用 using 声明（`using std::format;`、`using asio::co_spawn;`、`using runtime::Task;` 等）和命名空间别名（`namespace fs = std::filesystem;`），代码里避免写全限定名；`std::move`、`std::forward` 保留限定（Clang 对非限定调用告警），与本层同名的外层类型（如服务里的 `storage::Error`）和自定义 `Exception` 的基类 `std::exception` 保留限定。不改回标准库头文件方案，也不假设工具链提供可供外部导入的 `std:vector` 等逐类型分区。Asio、MMKV、Qt 的头文件仍按其工具链要求使用，并放在 `import std;` 之前（模块单元放在全局模块片段）；MSVC STL 不支持先 import 后 include。
- 灵活使用 `auto`、`const auto`、引用和有意义的类型别名，避免重复长类型名；公共接口的返回契约、数值宽度和资源所有权保持清晰。
- 异步接口返回 `runtime::Task<T>`（`asio::awaitable<T>` 的别名），内部用 `co_await`、`co_return`；调用方在任何协程里直接 `co_await`，不需要知道执行器。业务接口不接收完成回调。只有 Qt 边界（`viewmodels::spawn`）、ApplicationContext 启动 async_main 与测试用 `co_spawn` 从同步代码启动任务，测试可以使用 `use_future`。
- Asio 公共运行时放在 `runtime`，由 ApplicationContext 创建并统一管理其生命周期。运行时管理 `io_context`、work guard 与唯一的后台线程。协程词汇 `runtime::Task`、`runtime::Executor` 定义在 `runtime/task.h`，用到它们的接口与实现单元在全局模块片段包含该头文件，像包含其他 Asio 头文件一样，不为此导入 `Template.Runtime.Asio`；只有需要 AsioRuntime 本身的单元（ApplicationContext、测试）导入运行时模块。业务服务接收注入的执行器，不创建自己的事件循环或线程，不依赖其他业务服务获取公共运行时。
- 服务基于注入的执行器创建自己的 strand，不对外暴露执行器。公共方法把操作及其参数连同服务的共享状态交给 `onStrand`，由它用 `co_spawn(strand, …, use_awaitable)` 在 strand 上运行，调用方在自己的执行器上恢复；只做同步处理的操作写成同步 Result 函数（TaskService），需要等待 I/O 的操作写成 `Task` 协程（导入服务）；同一服务的操作按提交顺序在 strand 上执行，业务状态只在 strand 上访问。MMKV 的同步操作放在运行时后台线程执行。
- `network::HttpClient` 用 cofetch 发送请求，构造时接收 `AsioRuntime::context()` 返回的 `io_context`；cofetch 不是线程安全的，且完成时直接调用处理器，因此 HttpClient 在 `io_context` 上运行传输，再经 `co_spawn` 让调用方回到自己的执行器。传输失败与取消返回 `network::Error`，任何 HTTP 状态都作为 `Response` 返回，由服务决定可接受的状态。ApplicationContext 创建共享 HttpClient 注入需要远程数据的服务，退出时先 `stop()` 中止进行中的请求，再等待运行时排空；HttpClient 在运行时线程上或运行时结束后销毁。
- 已创建的业务协程持有共享状态，服务对象销毁后仍可完成。公共运行时必须活过所有使用者；停止提交后在应用所属线程调用 `finish()` 或析构，等待已提交操作完成。长期 I/O 需由所属业务先取消。
- 应用级异步入口为 `app/asyncmain/` 的 `Template.App.AsyncMain`，使用 `async_main(Dependencies) -> runtime::Task<>`；模块不依赖 Qt，归入 template_core，可在无 Qt 配置下验证。AsioRuntime 管理运行时资源，ApplicationContext 装配依赖，async_main 按顺序执行 Dependencies 中的启动步骤并编排退出清理；async_main 不依赖具体业务服务，新增功能不修改该模块。
- ApplicationContext 构造时只装配，main 加载 QML 后调用一次 start()，再运行 Qt app.exec()。首次业务加载由 async_main 发起；ViewModel 构造时不自动 reload，通过 Initialization 接收启动结果，独立使用时显式调用 reload()。
- Lifecycle 的启动结果与退出通知使用 Asio 通道，不给业务接口添加完成回调；每个需要启动加载的功能通过 `lifecycle->startup<Result>()` 登记一个只有单一消费者的 `Startup<Result>`，由 `startup_step` 直接 `co_await` 服务的加载操作并交付结果，退出或启动失败时统一取消。ApplicationContext 用 `attach(viewModel, service, &Service::operation)` 为功能接线启动步骤、`initialize()` 与 `stop()`；没有启动加载的 ViewModel 用 `attach(viewModel)`，只在退出时接收 `stop()`。退出通知可早于协程等待；stop() 幂等并拒绝新的界面命令，aboutToQuit 与析构都通知退出。Impl 析构先请求退出，在 ViewModel 和服务仍存在时 finish() 等待后台操作与线程结束，再按成员顺序释放资源；退出清理不等待 GUI 回调。新增持续 I/O 时由所属业务响应退出、取消操作，并在 async_main 返回前等待清理完成。

## 错误处理

- 错误处理统一使用 `errors/errors.cppm` 的 `Template.Errors`，写法对照 Rust：`errors::Result<T, Code>` 派生自 `std::expected`，补充 `is_ok`、`is_err`、`map`、`map_err`、`and_then`、`unwrap_or`、`expect`、`context`；成功用 `Ok(值)`（`Result<void>` 用 `Ok()`），失败用 `Err(code, "格式串", 参数…)`。各层（`business`、`storage`、`network`、`application`）只定义自己的 `enum class ErrorCode`，再用 `using Error = errors::Error<ErrorCode>;`、`template <class T> using Result = errors::Result<T, ErrorCode>;` 与需要时的 `using Exception = errors::Exception<ErrorCode>;` 引用共享模板，不再各写一份。业务失败、校验失败、I/O 与格式错误都返回 Result，不抛异常。
- 返回 Result 的同步函数里，`co_await r` 就是 Rust 的 `r?`：成功时得到值，失败时函数立即返回该错误；用 `co_return` 返回成功值或 `Err(…)`。异步协程（`runtime::Task`）只 `co_await` 异步操作，Asio 在编译期拒绝对 Result 使用 co_await；需要 `?` 的逻辑写成同步 Result 函数，由异步函数调用（如 TaskService 的各项操作、导入服务的 `importTasks`）。这种协程依赖 Clang 在协程首次返回调用方时才转换返回对象，本项目只使用 Clang。
- 跨层错误通过特化 `errors::From<目标层 ErrorCode, 源层 ErrorCode>`（静态函数 `code` 映射错误码）转换，相当于 Rust 的 `impl From`；特化写在用到转换的单元里（TaskService 的 storage → business、导入服务的 network → business）。之后 `?`、`Err` 返回和 `and_then` 都会自动转换，保留原有 detail。
- `Error::detail` 按 `<上下文>: <原因>` 书写，由外向内列出操作、对象（任务 ID、键、MMKV 实例与目录、记录序号、请求的方法与 URL）和原因；向上传递时用 Result 或 Error 的 `.context("外层操作")` 加上外层上下文，不丢弃内层信息。
- 只在构造失败（缺少依赖）和调用方违反使用约定（重复初始化或重复启动）时抛出所在层的 `Exception`，它携带同样的 `ErrorCode` 与带上下文的 `Error`；不抛标准库异常。`expect()` 相当于 Rust 的 panic，失败时抛出 `Exception`，只用于违反约定的场景与测试。退出导致的启动结果取消是正常流程，返回空结果。
- 第三方库的失败在边界转换：MMKV SDK 抛出的异常在 storage 内转为 `storage::Error`，只有 `std::bad_alloc` 继续传播；libcurl 的传输错误与取消在 network 内转为 `network::Error`，服务再转为业务层的 `network` 或 `cancelled` 错误；业务服务不捕获异常，到达 ViewModel 与 ApplicationContext 的异常按不可恢复处理，在 Qt 边界显示或记录。

## 模板范围与持久化

- 固定使用 MMKV、Asio、cofetch 与 glaze，直接依赖具体实现；不为假设的后端替换引入 Repository、Runtime、HTTP 抽象接口、工厂或适配层。
- MMKV 代码集中在 `storage/`，由通用 MmkvStore 负责键值读写与句柄管理；任务键、任务记录格式和业务校验由 TaskService 负责；AsioRuntime 位于 `runtime/`，保留集中管理运行时资源的职责。依赖注入用于装配和生命周期管理。
- 存储与业务测试使用临时目录中的真实 MMKV，网络测试使用测试进程内的回环 HTTP 服务，不访问外网，也不为测试新增可替换的存储或 HTTP 接口。
- cofetch 与 libcurl 的头文件集中在 `network/httpclient_sdk.cppm` 的私有实现分区 `:sdk`，glaze 集中在服务的私有分区（如 `services/taskimportservice/taskimportservice_json.cppm` 的 `:json`）；这些分区的全局模块片段只包含第三方与所需标准库头文件，不 `import std`，公共接口不导出它们。glaze 反射的类型放在具名命名空间，不放在匿名命名空间。
- 持久化后端使用 MMKV 官方 C++ Core，SDK 头文件集中在 `storage/mmkv_sdk.cppm` 的私有实现分区，全局模块片段保持 include-before-import；`storage/mmkv.cpp` 导入 `:sdk` 使用原生类型。公共 API 只使用标准库类型与独立的 storage::Error，不导入任务或其他业务模型，也不导出 SDK 分区。
- ApplicationContext 创建共享 MmkvStore 并注入服务；不同业务使用具名键（如 settings.theme、tasks.items），按需使用独立的实例 ID。任务示例的编解码在 TaskService 中完成。
- JSON 只用 glaze 编解码 HTTP 数据；持久化仍用 MMKV 的原生类型，不改用 JSON。保持模板简洁，不加入旧格式兼容或数据迁移代码，不恢复独立的 `business_main.cpp` 示例入口。
- 持久化成功后提交业务状态，失败保留已提交状态；新增成功后才清空界面输入。
- 保存期间只锁定受影响的界面：不同任务的命令可以并发，服务 strand 按顺序执行并返回完整快照；保存中的任务、进行中的新增与加载各自拒绝新命令。被拒绝的命令返回 false 并发出带原因的 `commandRejected`，界面据此提示，不整体禁用。
- 操作错误按加载、新增、具体任务分别保留并汇总显示；成功只清除相同目标的错误。更新与删除同一任务属于同一目标，其他目标的成功不能清掉尚未解决的错误。任务不再出现在新的列表快照中时，它的错误随之清除，因为已无法针对它重试。

## 验证与编辑器

- 与 Modules、QObject 或 QML 注册有关的改动，验证受影响的 C++23/C++26 构建、ViewModel/QML 测试及 qmllint；涉及业务边界时检查无 Qt 构建。
- 保持 Zed 的 clangd Modules 配置；CMake 在构建目录导出 `compile_commands.json`，包含标准库和命名模块的编译命令，`.clangd` 读取 `build/debug` 的数据库。clangd 23 对 C++ Modules 的支持仍不稳定，模板不保证 clangd 无诊断，CI 也不检查；`check_clangd` 目标保留为排查工具。
- Qt Creator 从 CMake file API 在 `<构建目录>/.qtc_clangd/` 生成自己的编译数据库，不读取 `.clangd` 指定的数据库；clangd 通过环境变量 `CLANGD_FLAGS=--experimental-modules-support` 开启 Modules 支持。构建复制到仓库根目录的 `.qmlls.ini` 含本机路径，不提交。
- Clang 默认生成精简 BMI，只保留模块引用的全局模块片段声明。导出模板在导入方实例化时按 ADL 或特化查找的全局模块片段实体，须在模块内显式引用（参见 `asyncmain.cppm` 的 `channelError`），不在使用方补 `#include` 绕过。
- 不提交 BMI、构建产物或包含本机路径的编译数据库。验证结果区分本机检查与实际运行过的远端 CI。

<!-- astrlink-debug:begin -->
## AstrLink local data

AstrLink added this section with its agent debugging tools and removes it when they are uninstalled.

- Inspect AstrLink only through the read-only CLI that the `astrlink-debug` skill describes.
- Do not read, copy, search, or open AstrLink's data directory (`/Users/tschen/Library/Application Support/com.astrlink.desktop`), any `astrlink.db*` file, or `~/.astrlink/control-session.json`, and do not run `sqlite3` on them.
- The control socket and the session token only carry observer access. Do not use them to change AstrLink settings.
<!-- astrlink-debug:end -->
