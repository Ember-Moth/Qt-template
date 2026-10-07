# Qt Template

Windows、macOS、Linux 的 Qt QML + MVVM 开发模板。默认统一使用 C++23，可切换 C++26；业务采用 C++ Modules、标准库、Asio 协程和 MMKV，Qt 类型止于 ViewModel 适配边界及外侧的 UI 与应用启动代码。

待办示例展示依赖注入、应用级 `async_main()`、GUI 线程更新和持久化失败处理。模板固定使用 Asio 与 MMKV，扩展业务时直接注入具体服务和后端。

- [项目开发约定](AGENTS.md)：后续修改必须遵循的规则。
- [架构设计](docs/architecture.md)：模块边界、启动与退出流程、扩展方式。
- [构建入口](CMakeLists.txt)：目标、选项及源码自动发现；[CMakePresets.json](CMakePresets.json) 定义配置预设。

## 构建与运行

需要 CMake 4.4.x、Ninja、LLVM/Clang 23+ 以及 Qt 6.12+ 桌面 SDK。三个平台统一使用 Clang，标准库与该平台的 Qt SDK 一致：macOS 用 libc++，Linux 用 libstdc++，Windows 用 MSVC STL。编译器、标准库和 clangd 使用匹配版本。本机验证环境和远端 CI 状态见[验证](#验证)。

`import std` 在 CMake 4.4 中仍是实验功能：[cmake/ImportStd.cmake](cmake/ImportStd.cmake) 设置的开关 UUID 只对应 CMake 4.4.x，升级 CMake 时需同步更新。配置预设读取两个环境变量：`LLVM_ROOT` 指向 LLVM 安装目录，`QT_ROOT` 指向 Qt 桌面 SDK。

macOS 使用 Homebrew 的 LLVM 与其 libc++：

```sh
export LLVM_ROOT="$(brew --prefix llvm)"
export QT_ROOT="$HOME/Qt/6.12.0/macos"
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --build --preset debug --target lint
open build/debug/qt_template.app
```

Linux 使用 Clang 23 与系统 libstdc++ 15+（提供 `std` 模块源码）。Ubuntu 26.04 自带的 Clang 21 在同时包含 libstdc++ 头文件与 `import std;` 时报错，可从 [apt.llvm.org](https://apt.llvm.org) 安装 Clang 23：

```sh
export LLVM_ROOT=/usr/lib/llvm-23
export QT_ROOT="$HOME/Qt/6.12.0/gcc_64"
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --build --preset debug --target lint
./build/debug/qt_template
```

Windows 在 VS x64 开发者环境中使用 [LLVM 官方安装包](https://github.com/llvm/llvm-project/releases) 的 Clang 与 MSVC STL，Qt 使用 MSVC 套件（VS 2026 与 Qt 的 msvc2022_64 套件二进制兼容）。CMake 官方只支持 Clang 搭配 libc++ 或 libstdc++ 使用 `import std`，`cmake/ImportStd.cmake` 为 MSVC STL 的 `std.ixx` 生成 libc++ 格式的模块清单。运行应用前将 Qt 的 `bin` 目录加入 PATH：

```powershell
$env:LLVM_ROOT = "C:/Program Files/LLVM"
$env:QT_ROOT = "C:/Qt/6.12.0/msvc2022_64"
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
cmake --build --preset debug --target lint
$env:PATH = "$env:QT_ROOT/bin;$env:PATH"; .\build\debug\qt_template.exe
```

| 预设 | 内容 | 构建目录 |
| --- | --- | --- |
| `debug` | Debug、C++23，桌面应用与全部测试 | `build/debug` |
| `release` | Release、C++23 | `build/release` |
| `debug-cxx26` | Debug、C++26 | `build/debug-cxx26` |
| `core` | Debug、C++23，只构建无 Qt 部分及其测试 | `build/core` |

每个预设使用独立构建目录，配置保存在其 `CMakeCache.txt`，切换预设不影响其他预设。配置时加 `-DTEMPLATE_TESTS=OFF` 关闭测试目标。

Asio 1.38.2 与 MMKV 2.4.2 的源码地址和 SHA-256 固定在 [cmake/Dependencies.cmake](cmake/Dependencies.cmake)，由 FetchContent 下载到构建目录的 `_deps/`。MMKV 官方 C++ Core 直接编译为 `template_mmkv`，保留加密支持并内置 zlib；模板的存储 API 当前使用单进程、未加密实例。

## 目录与模块

```text
src/
├── models/task/                  # Template.Models：任务数据与业务规则
│   ├── task.cppm
│   └── task.cpp
├── storage/                      # Template.Storage.Mmkv：通用持久化后端
│   ├── mmkv.cppm
│   ├── mmkv.cpp
│   └── mmkv_sdk.cppm              # 私有 SDK 实现分区
├── services/taskservice/          # Template.Tasks：协程业务接口
│   ├── taskservice.cppm
│   └── taskservice.cpp
├── runtime/                      # Template.Runtime.Asio：运行时资源
│   ├── asioruntime.cppm
│   └── asioruntime.cpp
├── viewmodels/
│   ├── tasklistmodel/             # Template.ViewModels.TaskList
│   └── taskviewmodel/             # Template.ViewModels.Task
├── ui/
│   ├── Main.qml
│   ├── Theme.qml
│   ├── components/
│   ├── pages/
│   └── qmltypes.h                 # QML 类型注册
└── app/
    ├── main.cpp                  # Qt 入口与根组件注入
    ├── asyncmain/                # Template.App.AsyncMain：无 Qt 的异步入口
    └── applicationcontext/       # Template.App.Context：装配与 ViewModel 集合
```

业务、ViewModel 和应用模块按模块建立子目录。`storage` 与 `runtime` 直接作为各自唯一后端的模块目录。接口、实现和可选分区放在同一目录，模块可以包含多个实现文件。

CMake 用 `file(GLOB_RECURSE … CONFIGURE_DEPENDS)` 按分层发现 `.cppm`（作为模块文件集）与 `.cpp`，Qt 层同时收集 `.h` 交给 AUTOMOC；新增文件在下次构建时自动重新配置。`qt_add_qml_module` 把 `src/ui` 的 QML 收集为 `Template.Ui` 模块，生成类型注册、qmldir 与 qmllint 目标，并由 qmlcachegen 预先编译为 C++，运行时不再解析 QML 源码。`Template.Ui` 构建为静态插件，应用与 QML 测试用 `Q_IMPORT_QML_PLUGIN(Template_UiPlugin)` 导入。模块的逻辑名称由 `export module` 决定。新增文件不需要逐个登记；额外库依赖仍需显式声明。QML 类型和依赖装配按[扩展步骤](docs/architecture.md#扩展业务与-viewmodel)更新。

| 目标 | 内容 |
| --- | --- |
| `template_core` | 模型、MMKV、服务、Asio 运行时、`app/asyncmain`；不依赖 Qt |
| `template_gui`、`template_guiplugin` | ViewModel、ApplicationContext 与 `Template.Ui` QML 模块及其静态插件 |
| `qt_template` | Qt 桌面应用 |
| `test_asyncmain`、`test_storage`、`test_business` | 无 Qt 测试 |
| `test_viewmodel`、`test_qml` | Qt 适配与 QML 交互测试 |

## 启动、界面与退出

`ApplicationContext` 构造时装配运行时、存储、服务、Lifecycle 和 ViewModel。main 加载 QML 后调用 `context.start()`，再运行 Qt 的 `app.exec()`。

`async_main(Dependencies)` 在公共 Asio 运行时上执行，按顺序运行启动步骤：每一步在服务自己的执行器上 `co_spawn` 并 `co_await` 加载，再交付给该功能登记的 `Startup<Result>`，随后根协程等待退出通知。ApplicationContext 用一行 `attach(viewModel, service, &Service::operation)` 为每个功能接线，新增功能无需修改 `app/asyncmain`。ViewModel 构造时不自动加载；`initialize()` 接收结果，在 GUI 线程更新列表、ready、busy 和错误信息。独立使用 ViewModel 时显式调用 `reload()`。

QML 类型注册在 `ui/qmltypes.h`，实例由 ApplicationContext 持有。main 只注入 `appContext`；根组件将 `appContext.tasks` 传给页面，页面通过 `required property TaskViewModel viewModel` 声明依赖。保存期间只锁定受影响的任务行或新增输入，其余界面保持可用；被拒绝的命令通过 `commandRejected` 在页面上短暂提示原因。

退出时 `aboutToQuit` 调用 `context.stop()`，拒绝新的界面命令并通知根协程。析构再次请求停止，调用 `finish()` 排空已接受操作并等待线程结束，再释放成员。清理不等待 GUI 回调，Qt 事件循环结束后仍能完成。示例使用有限操作；接入持续 I/O 时，所属业务需在退出请求到达后取消操作，并在根协程返回前等待清理完成。

## 错误处理

可恢复的失败通过 `std::expected` 返回，错误码为各层的 `enum class ErrorCode`，错误信息按 `<上下文>: <原因>` 由外向内带上操作、任务 ID、键与 MMKV 实例。只有构造失败和违反使用约定时抛出 `business::Exception` 或 `application::Exception`；退出导致的启动取消返回空结果。规则见 [AGENTS.md](AGENTS.md#错误处理)，设计见[架构文档](docs/architecture.md#错误处理)。

## 持久化

[通用 MmkvStore](src/storage/mmkv.cppm) 提供 bool、int64/uint64、double、字符串、字节数组、字符串列表读写，以及 `contains`、`keys`、`remove`、`clear`。公共接口使用标准库类型和独立的 `storage::Error`，任务键与编解码留在 TaskService。

getter 返回 `expected<optional<T>, storage::Error>`：缺失键为空 optional，空字符串、空数组和 false 仍是有效值。使用匹配的 setter/getter 类型；后端不维护额外的逐键类型标签。业务调用这些同步操作时使用 Asio 后台执行器。

数据位于应用数据目录的 `mmkv/`，默认实例 ID 为 `app`。不同业务可以共享一个 MmkvStore，使用 `settings.theme`、`tasks.items` 等具名键，或使用独立实例 ID。任务示例以字符串列表保存每条记录的 ID、标题和完成标记。

实例 ID 使用可移植 ASCII 字母、数字、下划线、连字符和点，排除空值、单独的 `.` 与 `..`。相同目录和实例 ID 共享句柄与操作锁，最后一个引用释放时关闭。业务先保存候选数据，再提交内存状态；保存失败保留已提交状态，新增成功后才清空界面输入。

模板不包含 JSON 兼容、历史数据迁移、可替换后端抽象或独立的业务演示程序。

## Zed / clangd 与增量构建

`.zed/settings.json` 为 `.cppm`、`.ixx` 启用 C++ 与 clangd Modules 支持。使用匹配工具链的 clangd，将其加入 Zed 的 PATH，或在个人设置的 `lsp.clangd.binary.path` 指定绝对路径。[Zed clangd 配置](https://zed.dev/docs/languages/cpp#binary)。

CMake 在每个构建目录导出 `compile_commands.json`，覆盖命名模块（通过 `@…modmap` 响应文件）和标准库模块。`.clangd` 读取 `build/debug` 的编译数据库，并让 clangd 自建一致的 BMI；使用其他预设时修改 `.clangd` 的 `CompilationDatabase`。编译数据库包含本机路径，位于被 Git 忽略的 `build/`；切换标准或目录后重新配置，再重启语言服务器。

`cmake --build --preset debug --target check_clangd` 使用 `LLVM_ROOT` 中的 clangd 检查项目全部 C++ 源码与模块的解析、类型和索引诊断；不执行逐位置的重构功能自检（泛型 auto 无法展开成具体类型）。MMKV SDK 头文件保留在私有 `:sdk` 实现分区的全局模块片段，存储实现导入该分区，避免 clangd 同时解析 SDK 标准库头文件和 import std 时的类型歧义；Windows 仍保持 include-before-import。

业务使用 `import std;`，按需声明 `using std::具体类型`。不使用 `using namespace std` 或假定存在 `std:vector` 等外部逐类型分区。Qt、Asio、MMKV 头文件按其工具链要求接入。

CMake 与 Ninja 用 clang-scan-deps 扫描模块依赖：头文件、模块接口或实现分区变化时，只重新编译依赖它们的 BMI 与对象。每个模块的 BMI 由声明它的目标编译一次，其他目标共享。Clang 默认生成精简 BMI，只保留模块引用到的全局模块片段声明；导入方实例化模板时按 ADL 查找的声明（如 Asio 通道错误码的 `make_error_code`）需在模块内显式引用，见 `asyncmain.cppm` 的 `channelError`。`qt_add_executable` 在 Qt 自己的策略作用域内创建目标，CMP0155 尚未启用，因此这些目标显式开启模块扫描。生成的 BMI、构建产物和编译数据库不进入版本控制。

## 验证

| 检查 | 覆盖 |
| --- | --- |
| `test_asyncmain` | 首次加载、提前退出、失败与重试、异常传播、多个功能的独立启动结果、启动取消返回空结果、装配错误异常 |
| `test_storage` | 原生类型、空值、实例隔离、共享句柄、文件损坏保护、错误码与上下文 |
| `test_business` | 业务校验、MMKV 重启读取、保存失败、协程与运行时退出、带上下文的错误、构造失败与约定异常 |
| `test_viewmodel` | 注入、一次初始化、GUI 通知、退出前已接受操作、并发命令只锁定各自目标、并发错误按目标保留与清除 |
| `test_qml` | 类型注册、依赖传递、界面操作、失败时保留输入与状态、保存中只锁定所在行并提示被拒绝的命令、其他任务成功时仍显示新增失败 |
| `lint` 目标 | `qt_add_qml_module` 生成的 qmllint 检查全部 QML 文件 |
| `check_clangd` 目标 | 当前预设中项目 C++ 源码、模块及 MMKV SDK 私有分区的编辑器诊断 |

本机已使用 macOS ARM64、CMake 4.4.4、Ninja 1.13.2、LLVM/libc++ 23.1.2、Qt 6.12.0 验证：`debug`、`debug-cxx26`、`release` 预设各 7 项 CTest 测试通过，`core` 预设 5 项通过，构建无警告；qmllint、`check_clangd`（18 个源文件）、qmlcachegen 预编译单元的运行时使用，以及头文件与模块接口变更的增量构建检查通过。

[GitHub Actions](.github/workflows/ci.yml) 使用 CMake 4.4.4 与 Ninja，在 Windows、macOS、Linux 上运行 `debug` 与 `debug-cxx26` 预设，并在 Linux 上运行无 Qt 的 `core` 预设；统一使用 LLVM 23，Qt SDK 为 6.12.0，Windows 使用 VS 2026 的 MSVC STL。迁移到 CMake 后的远端 CI 结果以最新一次运行为准。

## 复用模板

修改应用名称、版本、组织身份、QML URI `Template.Ui`、模块名称与示例页面。业务和应用级异步入口保持 Qt 无关，服务通过注入的执行器运行，界面状态在 GUI 线程更新。新功能按[架构文档的扩展步骤](docs/architecture.md#扩展业务与-viewmodel)接入。
