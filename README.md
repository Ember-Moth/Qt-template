# Qt Template

Windows、macOS、Linux 的 Qt QML + MVVM 开发模板。默认统一使用 C++23，可切换 C++26；业务采用 C++ Modules、标准库、Asio 协程和 MMKV，Qt 类型止于 ViewModel 适配边界及外侧的 UI 与应用启动代码。

待办示例展示依赖注入、应用级 `async_main()`、GUI 线程更新和持久化失败处理。模板固定使用 Asio 与 MMKV，扩展业务时直接注入具体服务和后端。

- [项目开发约定](AGENTS.md)：后续修改必须遵循的规则。
- [架构设计](docs/architecture.md)：模块边界、启动与退出流程、扩展方式。
- [构建入口](xmake.lua)：目标、配置选项及源码自动发现。

## 构建与运行

需要 xmake 3.1.1+、LLVM/Clang 23+ 以及 Qt 6.12+ 桌面 SDK。三个平台统一使用 Clang，标准库与该平台的 Qt SDK 一致：macOS 用 libc++，Linux 用 libstdc++，Windows 用 MSVC STL。编译器、标准库和 clangd 使用匹配版本。以下平台命令对应仓库的工具链配置；本机验证环境和远端 CI 状态见[验证](#验证)。

macOS 使用 LLVM/libc++：

```sh
export LLVM_ROOT="$(brew --prefix llvm)"
export QT_ROOT="$HOME/Qt/6.12.0/macos"
export PATH="$LLVM_ROOT/bin:$PATH"
xmake f -y -m debug --toolchain=llvm --sdk="$LLVM_ROOT" --qt="$QT_ROOT" --gui=y --tests=y --cxxstd=23 --builddir=build/xmake/cxx23
xmake build qt_template
xmake test
xmake lint
xmake run qt_template
```

Linux 使用 Clang 23 与系统 libstdc++ 15+（提供 `std` 模块源码）。Ubuntu 26.04 自带的 Clang 21 在同时包含 libstdc++ 头文件与 `import std;` 时报错，可从 [apt.llvm.org](https://apt.llvm.org) 安装 Clang 23。`LLVM_ROOT` 指向 LLVM 安装目录（如 `/usr/lib/llvm-23`），`QT_ROOT` 指向本机 SDK 目录：

```sh
xmake f -y -m debug --toolchain=llvm --sdk="$LLVM_ROOT" --qt="$QT_ROOT" --gui=y --tests=y --cxxstd=23 --builddir=build/xmake/cxx23
xmake build qt_template
xmake test
xmake lint
xmake run qt_template
```

Windows 在 VS x64 开发者环境中使用 [LLVM 官方安装包](https://github.com/llvm/llvm-project/releases) 的 Clang 与 MSVC STL，Qt 使用 MSVC 套件（VS 2026 与 Qt 的 msvc2022_64 套件二进制兼容）：

```powershell
$env:LLVM_ROOT = "C:/Program Files/LLVM"
$env:QT_ROOT = "C:/Qt/6.12.0/msvc2022_64"
xmake f -y -m debug --toolchain=llvm --sdk="$env:LLVM_ROOT" --qt="$env:QT_ROOT" --gui=y --tests=y --cxxstd=23 --builddir=build/xmake/cxx23
xmake build qt_template
xmake test
xmake lint
xmake run qt_template
```

切换配置时写出全部选项：`xmake f` 只要带选项就不沿用上次的配置，未写出的模式、工具链、SDK 与 Qt 会回到默认值（release 模式、不指定 LLVM SDK），`xmake clean -a` 也会删除配置。以下命令沿用上文的 `LLVM_ROOT` 与 `QT_ROOT`，Windows 改用 `$env:LLVM_ROOT` 与 `$env:QT_ROOT`。

切换桌面 C++26：

```sh
xmake f -y -m debug --toolchain=llvm --sdk="$LLVM_ROOT" --qt="$QT_ROOT" --gui=y --tests=y --cxxstd=26 --builddir=build/xmake/cxx26
xmake build qt_template
xmake test
xmake lint
```

单独构建和测试无 Qt 部分，包含业务与应用级协程入口：

```sh
xmake f -y -m debug --toolchain=llvm --sdk="$LLVM_ROOT" --qt="$QT_ROOT" --gui=n --tests=y --cxxstd=23 --builddir=build/xmake/business
xmake build template_core
xmake test
```

恢复桌面 C++23：

```sh
xmake f -y -m debug --toolchain=llvm --sdk="$LLVM_ROOT" --qt="$QT_ROOT" --gui=y --tests=y --cxxstd=23 --builddir=build/xmake/cxx23
xmake build qt_template
```

`--tests=n` 关闭测试目标；`xmake lint` 需要 `--gui=y`。本机配置保存在 `.xmake/`，各标准与模式使用独立构建目录，配置和构建按顺序执行。

Asio 1.38.2 与 MMKV 2.4.2 的源码版本和 SHA-256 固定在 [xmake/dependencies.lua](xmake/dependencies.lua)，首次配置下载到 `build/_deps/`。MMKV 官方 C++ Core 由 xmake 直接编译，保留加密支持并内置 zlib；模板的存储 API 当前使用单进程、未加密实例。

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

xmake 自动发现既有分层下的 `.cppm`、`.cpp`，Qt 层同时发现 `.h`，QML 资源自动收集，并由 qmlcachegen 预先编译为 C++ 随 `template_gui` 链接，运行时不再解析 QML 源码。模块的逻辑名称由 `export module` 决定。新增文件不需要逐个登记；额外库依赖仍需显式声明。QML 类型和依赖装配按[扩展步骤](docs/architecture.md#扩展业务与-viewmodel)更新。

| 目标 | 内容 |
| --- | --- |
| `template_core` | 模型、MMKV、服务、Asio 运行时、`app/asyncmain`；不依赖 Qt |
| `template_gui` | ViewModel、ApplicationContext、QML 注册与资源 |
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

xmake 构建后自动导出根目录 `compile_commands.json`，覆盖命名模块和标准库模块。`.clangd` 让 clangd 自建一致的 BMI。编译数据库包含本机路径，已被 Git 忽略；切换标准或目录后重新构建，再重启语言服务器。

运行 `xmake check-clangd` 会先构建并刷新编译数据库，再使用当前 LLVM SDK 的 clangd 检查项目 C++ 源码与模块的解析、类型和索引诊断；不执行逐位置的重构功能自检（泛型 auto 无法展开成具体类型）。MMKV SDK 头文件保留在私有 `:sdk` 实现分区的全局模块片段，存储实现导入该分区，避免 clangd 同时解析 SDK 标准库头文件和 import std 时的类型歧义；Windows 仍保持 include-before-import。

业务使用 `import std;`，按需声明 `using std::具体类型`。不使用 `using namespace std` 或假定存在 `std:vector` 等外部逐类型分区。Qt、Asio、MMKV 头文件按其工具链要求接入。

[xmake/modules.lua](xmake/modules.lua) 在切换配置时清理旧模块映射，并按项目内的 `#include` 与 `import` 关系，只让依赖改动文件的 BMI 与对象失效；删除头文件或模块接口时全部失效。依赖目标的 BMI 在编译参数兼容时复用，未被导入的标准库模块（如 `std.compat`）被裁剪。生成的 BMI、构建产物和编译数据库不进入版本控制。

## 验证

| 检查 | 覆盖 |
| --- | --- |
| `test_asyncmain` | 首次加载、提前退出、失败与重试、异常传播、多个功能的独立启动结果、启动取消返回空结果、装配错误异常 |
| `test_storage` | 原生类型、空值、实例隔离、共享句柄、文件损坏保护、错误码与上下文 |
| `test_business` | 业务校验、MMKV 重启读取、保存失败、协程与运行时退出、带上下文的错误、构造失败与约定异常 |
| `test_viewmodel` | 注入、一次初始化、GUI 通知、退出前已接受操作、并发命令只锁定各自目标、并发错误按目标保留与清除 |
| `test_qml` | 类型注册、依赖传递、界面操作、失败时保留输入与状态、保存中只锁定所在行并提示被拒绝的命令、其他任务成功时仍显示新增失败 |
| `xmake lint` | 生成的 QML 类型信息与全部 QML 文件；警告使检查失败 |
| `xmake check-clangd` | 当前配置中项目 C++ 源码、模块及 MMKV SDK 私有分区的编辑器诊断 |

本机已使用 macOS ARM64、xmake 3.1.1、LLVM/libc++ 23.1.2、Qt 6.12.0 验证：C++23/26 桌面配置各 5 项测试通过，无 Qt 配置各 3 项测试通过；qmllint、clangd 与头文件/实现变更的增量构建检查通过。

[GitHub Actions](.github/workflows/ci.yml) 配置了 Windows、macOS、Linux 的 C++23/26 桌面构建与测试，统一使用 LLVM 23，Qt SDK 为 6.12.0，Windows 使用 VS 2026 的 MSVC STL。远端 CI 在 `c0b0860` 上运行，六个任务的构建、测试与 qmllint 全部通过，macOS 任务的 `xmake check-clangd` 也已通过。

## 复用模板

修改应用名称、版本、组织身份、QML URI `Template.Ui`、模块名称与示例页面。业务和应用级异步入口保持 Qt 无关，服务通过注入的执行器运行，界面状态在 GUI 线程更新。新功能按[架构文档的扩展步骤](docs/architecture.md#扩展业务与-viewmodel)接入。
