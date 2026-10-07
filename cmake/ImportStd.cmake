# Included before project(): enables `import std` for Clang on every platform.

# `import std` is experimental in CMake 4.4. The gate UUID changes with every CMake release, so the
# project pins CMake 4.4.x and updates this value when it moves to a newer CMake.
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "f35a9ac6-8463-4d38-8eec-5d6008153e7d")

# The LLVM installation selected by the preset (LLVM_ROOT) or found from the compiler name.
if(NOT TEMPLATE_LLVM_ROOT)
    if(DEFINED ENV{LLVM_ROOT})
        file(TO_CMAKE_PATH "$ENV{LLVM_ROOT}" llvm_root)
    else()
        find_program(cxx_compiler NAMES "${CMAKE_CXX_COMPILER}" clang++ NO_CACHE REQUIRED)
        cmake_path(GET cxx_compiler PARENT_PATH llvm_bin)
        cmake_path(GET llvm_bin PARENT_PATH llvm_root)
    endif()
    set(TEMPLATE_LLVM_ROOT "${llvm_root}" CACHE PATH "LLVM installation providing Clang and clangd")
endif()

# CMake's Clang support reads a libc++-format module manifest. Linux finds libstdc++'s manifest on its
# own; Homebrew LLVM keeps libc++'s outside the compiler's search path, and the MSVC STL ships a
# different format, so describe its std.ixx in the format CMake expects.
if(NOT CMAKE_CXX_STDLIB_MODULES_JSON)
    if(CMAKE_HOST_APPLE AND EXISTS "${TEMPLATE_LLVM_ROOT}/lib/c++/libc++.modules.json")
        set(CMAKE_CXX_STDLIB_MODULES_JSON "${TEMPLATE_LLVM_ROOT}/lib/c++/libc++.modules.json" CACHE FILEPATH "")
    elseif(CMAKE_HOST_WIN32 AND DEFINED ENV{VCToolsInstallDir})
        file(TO_CMAKE_PATH "$ENV{VCToolsInstallDir}" vc_tools)
        set(manifest "${CMAKE_BINARY_DIR}/msstl.modules.json")
        file(WRITE "${manifest}" "{\"version\":1,\"revision\":1,\"modules\":[\
{\"logical-name\":\"std\",\"source-path\":\"${vc_tools}/modules/std.ixx\",\"is-std-library\":true},\
{\"logical-name\":\"std.compat\",\"source-path\":\"${vc_tools}/modules/std.compat.ixx\",\"is-std-library\":true}]}")
        set(CMAKE_CXX_STDLIB_MODULES_JSON "${manifest}" CACHE FILEPATH "")
    endif()
endif()
