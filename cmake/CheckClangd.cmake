# cmake -DCLANGD=<clangd> -DDATABASE=<build dir> -DSOURCE_DIR=<project> -P CheckClangd.cmake
# Runs clangd's parse, type and index checks on every project C++ source and module unit.
file(GLOB_RECURSE sources "${SOURCE_DIR}/src/*.cpp" "${SOURCE_DIR}/src/*.cppm")
list(SORT sources)
set(failed "")
foreach(source IN LISTS sources)
    cmake_path(RELATIVE_PATH source BASE_DIRECTORY "${SOURCE_DIR}" OUTPUT_VARIABLE relative)
    message(STATUS "clangd: ${relative}")
    # Refactoring tweaks cannot expand generic auto parameters, so skip per-location checks.
    execute_process(
        COMMAND "${CLANGD}" "--check=${source}" "--compile-commands-dir=${DATABASE}"
                --experimental-modules-support --check-locations=false --log=error
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        list(APPEND failed "${relative}")
    endif()
endforeach()
if(failed)
    message(FATAL_ERROR "clangd reported errors in: ${failed}")
endif()
