cmake_minimum_required(VERSION 4.4)
include(cmake/ImportStd.cmake)
project(QtTemplate VERSION 0.1.0 LANGUAGES C CXX)

set(TEMPLATE_CXX_STANDARD 23 CACHE STRING "C++ standard: 23 or 26")
set_property(CACHE TEMPLATE_CXX_STANDARD PROPERTY STRINGS 23 26)
option(TEMPLATE_GUI "Build the Qt layers, application and QML tests" ON)
option(TEMPLATE_TESTS "Build the tests" ON)

set(CMAKE_CXX_STANDARD ${TEMPLATE_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "Build with Clang on every platform; see README")
endif()
if(NOT TEMPLATE_CXX_STANDARD IN_LIST CMAKE_CXX_COMPILER_IMPORT_STD)
    message(FATAL_ERROR "import std is unavailable for C++${TEMPLATE_CXX_STANDARD}: ${CMAKE_CXX_COMPILER_IMPORT_STD_ERROR_MESSAGE}")
endif()

if(APPLE)
    # Homebrew LLVM with its own libc++; the system libc++ predates the standard library modules.
    string(APPEND CMAKE_CXX_FLAGS " -stdlib=libc++")
    set(libcxx "${TEMPLATE_LLVM_ROOT}/lib/c++")
    foreach(kind EXE SHARED MODULE)
        string(APPEND CMAKE_${kind}_LINKER_FLAGS " -stdlib=libc++ -L${libcxx} -Wl,-rpath,${libcxx}")
    endforeach()
endif()
# Standard library module sources trip these warnings on every platform.
add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-Wno-include-angled-in-module-purview;-Wno-reserved-module-identifier>")

include(cmake/Dependencies.cmake)

set(SRC "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(TESTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")

# Sources are discovered per layer, so adding a module to an existing layer needs no build change.
function(template_add_layers target)
    foreach(layer IN LISTS ARGN)
        file(GLOB_RECURSE interfaces CONFIGURE_DEPENDS "${SRC}/${layer}/*.cppm")
        file(GLOB_RECURSE sources CONFIGURE_DEPENDS "${SRC}/${layer}/*.cpp")
        list(FILTER sources EXCLUDE REGEX "/app/main\\.cpp$")
        target_sources(${target} PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${SRC}" FILES ${interfaces})
        target_sources(${target} PRIVATE ${sources})
    endforeach()
endfunction()

# qt_add_executable creates targets under Qt's own policy settings, where CMP0155 does not yet scan
# C++20 sources for imports; enable scanning so they can import project modules and std.
function(template_scan_modules)
    set_target_properties(${ARGN} PROPERTIES CXX_SCAN_FOR_MODULES ON)
endfunction()

# Models, storage, services, runtime and the application coroutine: no Qt.
add_library(template_core STATIC)
template_add_layers(template_core models storage services runtime app/asyncmain)
target_link_libraries(template_core PUBLIC template_asio PRIVATE template_mmkv)

if(TEMPLATE_GUI)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 Test)
    qt_standard_project_setup(REQUIRES 6.12)

    # ViewModels, ApplicationContext and the Template.Ui QML module.
    add_library(template_gui STATIC)
    template_add_layers(template_gui viewmodels app/applicationcontext)
    file(GLOB_RECURSE gui_headers CONFIGURE_DEPENDS "${SRC}/viewmodels/*.h" "${SRC}/app/applicationcontext/*.h")
    target_sources(template_gui PRIVATE ${gui_headers} "${SRC}/ui/qmltypes.h")
    # Generated moc and type registration sources include the QObject headers by file name.
    set(header_dirs "${SRC}" "${SRC}/ui")
    foreach(header IN LISTS gui_headers)
        cmake_path(GET header PARENT_PATH header_dir)
        list(APPEND header_dirs "${header_dir}")
    endforeach()
    list(REMOVE_DUPLICATES header_dirs)
    target_include_directories(template_gui PUBLIC ${header_dirs})
    target_link_libraries(template_gui PUBLIC template_core Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick)

    # QML types are named after their files and served flat from /qt/qml/Template/Ui, so file names
    # stay unique across src/ui. qmlcachegen compiles them ahead of time.
    file(GLOB_RECURSE qml_files CONFIGURE_DEPENDS "${SRC}/ui/*.qml")
    foreach(qml IN LISTS qml_files)
        cmake_path(GET qml FILENAME alias)
        set_source_files_properties("${qml}" PROPERTIES QT_RESOURCE_ALIAS "${alias}")
        file(READ "${qml}" content)
        if(content MATCHES "pragma Singleton")
            set_source_files_properties("${qml}" PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
        endif()
    endforeach()
    set(QT_QML_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/qml")
    qt_add_qml_module(template_gui URI Template.Ui VERSION 1.0 QML_FILES ${qml_files})

    qt_add_executable(qt_template WIN32 MACOSX_BUNDLE "${SRC}/app/main.cpp")
    template_scan_modules(qt_template)
    target_compile_definitions(qt_template PRIVATE APP_VERSION="${PROJECT_VERSION}")
    target_link_libraries(qt_template PRIVATE template_gui template_guiplugin Qt6::QuickControls2)

    # cmake --build --preset <name> --target lint
    add_custom_target(lint)
    add_dependencies(lint all_qmllint)
endif()

if(TEMPLATE_TESTS)
    enable_testing()
    foreach(name asyncmain storage business)
        add_executable(test_${name} "${TESTS}/test_${name}.cpp")
        target_link_libraries(test_${name} PRIVATE template_core template_mmkv)
        set_target_properties(test_${name} PROPERTIES AUTOMOC OFF)
        add_test(NAME ${name} COMMAND test_${name})
    endforeach()
    # A second and third process write and read the same MMKV store.
    set(process_store "${CMAKE_BINARY_DIR}/mmkv-process-store")
    add_test(NAME business_write COMMAND test_business --mmkv-write "${process_store}")
    add_test(NAME business_read COMMAND test_business --mmkv-read "${process_store}")
    set_tests_properties(business_write PROPERTIES FIXTURES_SETUP process_store)
    set_tests_properties(business_read PROPERTIES FIXTURES_REQUIRED process_store)

    if(TEMPLATE_GUI)
        qt_add_executable(test_viewmodel "${TESTS}/test_core.cpp" "${TESTS}/test_core.h")
        target_link_libraries(test_viewmodel PRIVATE template_gui template_mmkv Qt6::Test)
        qt_add_executable(test_qml "${TESTS}/test_qml.cpp" "${TESTS}/test_qml.h")
        target_link_libraries(test_qml PRIVATE template_gui template_guiplugin Qt6::Test Qt6::QuickControls2)
        template_scan_modules(test_viewmodel test_qml)
        add_test(NAME viewmodel COMMAND test_viewmodel)
        add_test(NAME qml COMMAND test_qml)
        set_tests_properties(viewmodel qml PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
    endif()
endif()

# cmake --build --preset <name> --target check_clangd: editor diagnostics for every source and module.
find_program(TEMPLATE_CLANGD clangd HINTS "${TEMPLATE_LLVM_ROOT}/bin" NO_DEFAULT_PATH)
if(TEMPLATE_CLANGD)
    add_custom_target(check_clangd
        COMMAND "${CMAKE_COMMAND}" "-DCLANGD=${TEMPLATE_CLANGD}" "-DDATABASE=${CMAKE_BINARY_DIR}"
                "-DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckClangd.cmake"
        USES_TERMINAL)
endif()
