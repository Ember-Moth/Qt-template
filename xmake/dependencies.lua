target("template_asio")
    set_kind("headeronly")
    on_load(function(target)
        local root = import("dependency_source", {rootdir = "xmake"})("asio", "1.38.2",
            "https://codeload.github.com/chriskohlhoff/asio/tar.gz/refs/tags/asio-1-38-2",
            "9f2648fa483e58a6bf848d970ee0ea650ca19ed7769dfa520ed4f7b8d27af1db")
        target:add("sysincludedirs", path.join(root, "include"), {public = true})
        target:add("defines", "ASIO_STANDALONE", {public = true})
        if target:is_plat("windows") then
            target:add("syslinks", "ws2_32", "mswsock", {public = true})
        elseif target:is_plat("linux") then
            target:add("syslinks", "pthread", {public = true})
        end
    end)
target_end()

target("template_mmkv")
    set_kind("static")
    set_policy("build.c++.modules", false)
    on_load(function(target)
        local root = path.join(import("dependency_source", {rootdir = "xmake"})("mmkv", "2.4.2",
            "https://codeload.github.com/Tencent/MMKV/tar.gz/refs/tags/v2.4.2",
            "e77d81daab0c905c06cad6bceb6ad01ca606a428987e2f1f41a2aa162592c685"), "Core")
        target:add("files", path.join(root, "*.cpp"), path.join(root, "aes/**.cpp"),
            path.join(root, "crc32/**.cpp"), path.join(root, "cbridge/*.cpp"))
        target:add("sysincludedirs", root, path.join(root, "include"), path.join(root, "cbridge"), {public = true})
        target:add("defines", "MMKV_EMBED_ZLIB=1", {public = true})
        local include = path.join(root, "include", "MMKV")
        os.mkdir(include)
        for _, name in ipairs({"MMKV.h", "MMKVPredef.h", "MMBuffer.h", "MiniPBCoder.h", "MMKVHandler.h"}) do
            os.cp(path.join(root, name), include)
        end
        if target:is_plat("macosx") then
            target:add("defines", "FORCE_POSIX", {public = true})
        elseif target:is_plat("windows") then
            target:add("defines", "UNICODE", "_UNICODE", {public = true})
            target:add("syslinks", "advapi32", {public = true})
        end
        if target:is_arch("arm64", "aarch64") and not target:is_plat("windows") then
            target:add("files", path.join(root, "aes/openssl/openssl_aesv8-armx.S"))
            target:add("asflags", "-march=armv8-a+crypto", {force = true})
        end
    end)
target_end()
