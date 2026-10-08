# Asio, MMKV, cofetch and glaze, pinned to release archives and their SHA-256 sums.
include(FetchContent)
FetchContent_Declare(asio
    URL https://codeload.github.com/chriskohlhoff/asio/tar.gz/refs/tags/asio-1-38-2
    URL_HASH SHA256=9f2648fa483e58a6bf848d970ee0ea650ca19ed7769dfa520ed4f7b8d27af1db
    SOURCE_SUBDIR do-not-add DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(mmkv
    URL https://codeload.github.com/Tencent/MMKV/tar.gz/refs/tags/v2.4.2
    URL_HASH SHA256=e77d81daab0c905c06cad6bceb6ad01ca606a428987e2f1f41a2aa162592c685
    SOURCE_SUBDIR do-not-add DOWNLOAD_EXTRACT_TIMESTAMP ON)
# cofetch's own CMakeLists requires find_package(CURL), which the Windows source build cannot satisfy.
FetchContent_Declare(cofetch
    URL https://codeload.github.com/SSARCandy/cofetch/tar.gz/refs/tags/v0.1.2
    URL_HASH SHA256=846fe452970cdc0f071aca017453f350eef789fddbf467bd6814bb9c4bcd3b42
    SOURCE_SUBDIR do-not-add DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_Declare(glaze
    URL https://codeload.github.com/stephenberry/glaze/tar.gz/refs/tags/v9.0.0
    URL_HASH SHA256=dd7b033c5bf6e4308615bb7834c541291d5b20f64121d9a68222a39414635517
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
FetchContent_MakeAvailable(asio mmkv cofetch glaze)

add_library(template_asio INTERFACE)
target_include_directories(template_asio SYSTEM INTERFACE "${asio_SOURCE_DIR}/include")
target_compile_definitions(template_asio INTERFACE ASIO_STANDALONE)
if(WIN32)
    target_link_libraries(template_asio INTERFACE ws2_32 mswsock)
elseif(UNIX AND NOT APPLE)
    target_link_libraries(template_asio INTERFACE pthread)
endif()

# The official MMKV C++ Core, built directly with encryption support and the bundled zlib.
set(mmkv_core "${mmkv_SOURCE_DIR}/Core")
file(GLOB mmkv_sources "${mmkv_core}/*.cpp" "${mmkv_core}/cbridge/*.cpp")
file(GLOB_RECURSE mmkv_nested_sources "${mmkv_core}/aes/*.cpp" "${mmkv_core}/crc32/*.cpp")
add_library(template_mmkv STATIC ${mmkv_sources} ${mmkv_nested_sources})
set_target_properties(template_mmkv PROPERTIES CXX_MODULE_STD OFF CXX_SCAN_FOR_MODULES OFF)
# ARM64 AES routines are referenced on Apple and Linux; MMKV leaves them out only for MSVC builds.
if(NOT WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    enable_language(ASM)
    set(mmkv_aes_asm "${mmkv_core}/aes/openssl/openssl_aesv8-armx.S")
    target_sources(template_mmkv PRIVATE "${mmkv_aes_asm}")
    set_source_files_properties("${mmkv_aes_asm}" PROPERTIES COMPILE_OPTIONS "-march=armv8-a+crypto")
endif()
# Consumers include the SDK as "MMKV/MMKV.h".
file(COPY "${mmkv_core}/MMKV.h" "${mmkv_core}/MMKVPredef.h" "${mmkv_core}/MMBuffer.h"
          "${mmkv_core}/MiniPBCoder.h" "${mmkv_core}/MMKVHandler.h"
     DESTINATION "${CMAKE_BINARY_DIR}/mmkv-include/MMKV")
target_include_directories(template_mmkv SYSTEM PUBLIC
    "${mmkv_core}" "${CMAKE_BINARY_DIR}/mmkv-include" "${mmkv_core}/cbridge")
target_compile_definitions(template_mmkv PUBLIC MMKV_EMBED_ZLIB=1
    $<$<PLATFORM_ID:Darwin>:FORCE_POSIX> "$<$<PLATFORM_ID:Windows>:UNICODE;_UNICODE>")
if(WIN32)
    target_link_libraries(template_mmkv PUBLIC advapi32)
endif()

# libcurl for cofetch: the system library on macOS and Linux, a static Schannel build on Windows,
# which ships no libcurl to link against. Each provides CURL::libcurl.
if(WIN32)
    FetchContent_Declare(curl
        URL https://github.com/curl/curl/releases/download/curl-8_22_0/curl-8.22.0.tar.xz
        URL_HASH SHA256=f7ef3ae8a22e521f289803fe93543eb64c329b58aa73a9e224dfd915a2a5f4f7
        DOWNLOAD_EXTRACT_TIMESTAMP ON EXCLUDE_FROM_ALL)
    block()
        # curl's options read these variables; CMP0126 keeps its AUTO cache defaults from replacing them.
        set(CMAKE_POLICY_DEFAULT_CMP0126 NEW)
        set(BUILD_SHARED_LIBS OFF)
        set(BUILD_STATIC_LIBS ON)
        set(BUILD_CURL_EXE OFF)
        set(BUILD_TESTING OFF)
        set(BUILD_EXAMPLES OFF)
        set(BUILD_LIBCURL_DOCS OFF)
        set(BUILD_MISC_DOCS OFF)
        set(ENABLE_CURL_MANUAL OFF)
        set(CURL_DISABLE_INSTALL ON)
        set(CURL_USE_SCHANNEL ON)
        set(CURL_USE_OPENSSL OFF)
        set(CURL_USE_LIBPSL OFF)
        set(CURL_USE_LIBSSH2 OFF)
        set(CURL_USE_PKGCONFIG OFF)
        set(CURL_ZLIB OFF)
        set(CURL_BROTLI OFF)
        set(CURL_ZSTD OFF)
        set(USE_NGHTTP2 OFF)
        set(USE_LIBIDN2 OFF)
        set(CURL_DISABLE_LDAP ON)
        FetchContent_MakeAvailable(curl)
    endblock()
else()
    find_package(CURL 7.80 REQUIRED)
    if(APPLE)
        # The SDK's curl headers are on the sysroot search path already; listing the SDK's usr/include
        # as -isystem would put the C headers ahead of libc++.
        set_property(TARGET CURL::libcurl PROPERTY INTERFACE_INCLUDE_DIRECTORIES "")
    endif()
endif()

add_library(template_cofetch INTERFACE)
target_include_directories(template_cofetch SYSTEM INTERFACE "${cofetch_SOURCE_DIR}/include")
target_link_libraries(template_cofetch INTERFACE template_asio CURL::libcurl)
