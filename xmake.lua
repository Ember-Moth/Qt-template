set_project("QtTemplate")
set_version("0.1.0")
set_xmakever("3.1.1")
add_rules("mode.debug", "mode.release")
add_rules("template.modules", "template.ide")

option("cxxstd")
    set_default("23")
    set_values("23", "26")
    set_showmenu(true)
option_end()
option("gui")
    set_default(true)
    set_showmenu(true)
option_end()
option("tests")
    set_default(true)
    set_showmenu(true)
option_end()

set_languages("c++" .. (get_config("cxxstd") or "23"))
set_config("builddir", "build/xmake/cxx" .. (get_config("cxxstd") or "23"))
set_policy("build.c++.modules", true)
set_policy("build.c++.modules.culling", false)
-- Keep each target's BMIs tied to its own flags and selected language standard.
set_policy("build.c++.modules.reuse", false)

-- Clang on every platform, with the C++ library of the platform's Qt SDK:
-- libc++ on macOS, libstdc++ on Linux and the MSVC STL on Windows.
set_toolchains("llvm")
if is_plat("macosx") then
    local llvm = get_config("sdk") or os.getenv("LLVM_ROOT")
    if llvm then
        set_toolset("ar", path.join(llvm, "bin/llvm-ar"))
        add_cxxflags("-stdlib=libc++", "-nostdinc++", {force = true})
        add_sysincludedirs(path.join(llvm, "include/c++/v1"))
        add_linkdirs(path.join(llvm, "lib/c++"))
        add_rpathdirs(path.join(llvm, "lib/c++"))
    end
    set_runtimes("c++_shared")
elseif is_plat("windows") then
    -- Qt GUI apps use the Windows subsystem with main(); xmake adds this entry only for link.exe.
    add_ldflags("-Wl,-entry:mainCRTStartup", {force = true})
end

includes("xmake/dependencies.lua", "xmake/qt.lua", "xmake/modules.lua", "xmake/ide.lua")

task("lint")
    on_run(function()
        import("core.project.project")
        import("core.base.task")
        import("lib.detect.find_file")
        task.run("build", {target = "template_gui"})
        local target = assert(project.target("template_gui"), "Enable --gui=y for QML lint")
        local qt = target:data("qt")
        local tool = assert(find_file(is_host("windows") and "qmllint.exe" or "qmllint", {qt.bindir, qt.libexecdir}))
        local args = {"--bare", "-I", qt.qmldir, "-I", path.join(target:autogendir(), "qml"),
            "-I", target:data("template.qml.directory"), "--resource", path.join(target:autogendir(), "template_ui.qrc")}
        table.join2(args, os.files("src/ui/**.qml"))
        os.execv(tool, args)
    end)
    set_menu({usage = "xmake lint", description = "Check QML against generated type information", options = {}})
task_end()

local function add_modules(layers, qt_headers)
    for _, layer in ipairs(layers) do
        local exclude = layer == "app" and "|asyncmain/**" or ""
        add_files("src/" .. layer .. "/**.cppm" .. exclude, {public = true})
        add_files("src/" .. layer .. "/**.cpp|main.cpp" .. exclude)
        if qt_headers then
            add_files("src/" .. layer .. "/**.h")
        end
    end
end

target("template_core")
    set_kind("static")
    add_deps("template_asio", "template_mmkv")
    add_modules({"models", "storage", "services", "runtime", "app/asyncmain"})
target_end()

if has_config("gui") then
    target("template_gui")
        add_rules("qt.static", "template.qml")
        add_deps("template_core", "template_asio")
        add_frameworks("QtCore", "QtGui", "QtQml", "QtQuick", "QtQuickControls2")
        add_includedirs("src", {public = true})
        add_modules({"viewmodels", "app"}, true)
        add_files("src/ui/qmltypes.h", {rules = "template.qml"})
        on_load(function(target)
            import("qml_resources", {rootdir = "xmake"}).generate(target)
        end)
    target_end()

    target("qt_template")
        add_rules("qt.quickapp")
        add_deps("template_gui")
        add_frameworks("QtQuickControls2")
        add_defines('APP_VERSION="0.1.0"')
        add_files("src/app/main.cpp")
    target_end()
end

if has_config("tests") then
    target("test_asyncmain")
        set_kind("binary")
        set_default(false)
        add_deps("template_core", "template_asio")
        add_files("tests/test_asyncmain.cpp")
        add_tests("asyncmain", {run_timeout = 30000})
    target_end()
    target("test_storage")
        set_kind("binary")
        set_default(false)
        add_deps("template_core")
        add_files("tests/test_storage.cpp")
        add_tests("storage", {run_timeout = 30000})
    target_end()
    target("test_business")
        set_kind("binary")
        set_default(false)
        add_deps("template_core", "template_asio", "template_mmkv")
        add_files("tests/test_business.cpp")
        add_tests("business", {run_timeout = 30000})
        on_test(function(target, opt)
            os.execv(target:targetfile(), {})
            local directory = path.join(target:autogendir(), "mmkv-process-store")
            os.execv(target:targetfile(), {"--mmkv-write", directory})
            os.execv(target:targetfile(), {"--mmkv-read", directory})
            return true
        end)
    target_end()
    if has_config("gui") then
        target("test_viewmodel")
            add_rules("qt.console")
            set_default(false)
            add_frameworks("QtTest")
            add_deps("template_gui", "template_core", "template_asio")
            add_files("tests/test_core.cpp", "tests/test_core.h")
            add_tests("viewmodel", {run_timeout = 30000})
        target_end()
        target("test_qml")
            add_rules("qt.quickapp")
            set_default(false)
            add_frameworks("QtTest", "QtQuickControls2")
            add_deps("template_gui")
            add_files("tests/test_qml.cpp", "tests/test_qml.h")
            add_runenvs("QT_QPA_PLATFORM", "offscreen")
            add_runenvs("QT_QUICK_BACKEND", "software")
            add_tests("qml", {run_timeout = 30000})
        target_end()
    end
end
