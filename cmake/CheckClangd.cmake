# cmake -DCLANGD=<clangd> -DDATABASE=<build dir> -DSOURCE_DIR=<project> -P CheckClangd.cmake
# Runs clangd's parse, type and index checks on the project sources this build compiles, read from
# its compilation database, so presets without the Qt layers skip the Qt sources.
file(READ "${DATABASE}/compile_commands.json" database)
string(JSON count LENGTH "${database}")
cmake_path(SET project_sources NORMALIZE "${SOURCE_DIR}/src/")
set(sources "")
math(EXPR last "${count} - 1")
foreach(index RANGE ${last})
    string(JSON file GET "${database}" ${index} file)
    string(JSON directory GET "${database}" ${index} directory)
    cmake_path(ABSOLUTE_PATH file BASE_DIRECTORY "${directory}" NORMALIZE)
    string(FIND "${file}" "${project_sources}" position)
    if(position EQUAL 0 AND file MATCHES "\\.(cpp|cppm)$")
        list(APPEND sources "${file}")
    endif()
endforeach()
list(REMOVE_DUPLICATES sources)
list(SORT sources)
if(NOT sources)
    message(FATAL_ERROR "No project sources found in ${DATABASE}/compile_commands.json")
endif()

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
