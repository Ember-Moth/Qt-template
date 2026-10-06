rule("template.modules")
    set_kind("project")
    before_build(function()
        import("core.project.config")
        import("core.cache.localcache")
        import("core.base.json")
        import("core.project.project")
        -- xmake 3.1.x keys its module mapper by target name, across build directories.
        -- Invalidate that mapper when switching configurations, before dependency scanning.
        local parts = {path.absolute(config.builddir())}
        for _, key in ipairs({"cxxstd", "mode", "toolchain", "sdk", "qt", "gui", "tests"}) do
            table.insert(parts, json.encode(config.get(key)))
        end
        local signature = table.concat(parts, "\n")
        local cache = localcache.cache("cxxmodules")
        if cache:get("template.configuration") ~= signature then
            cache:clear()
            cache:set("template.configuration", signature)
            cache:save()
        end

        -- xmake 3.1.x checks module source files, but omits global-fragment headers.
        -- Its importer metadata can also advance before the ordinary C++ object is compiled.
        -- Keep a per-configuration input snapshot and invalidate affected project artifacts.
        local inputs = localcache.cache("template-modules")
        local previous = inputs:get(signature) or {}
        local current, names, changed = {}, {}, {}
        local interfaces_changed = false
        for _, root in ipairs({"src", "tests"}) do
            for _, extension in ipairs({"cppm", "cpp", "h"}) do
                for _, file in ipairs(os.files(root .. "/**." .. extension)) do
                    current[file] = hash.sha256(file)
                    if current[file] ~= previous[file] then
                        changed[file] = true
                        interfaces_changed = interfaces_changed or extension ~= "cpp"
                    end
                    if extension == "cppm" then
                        local name = io.readfile(file):match("export%s+module%s+([%w_.:]+)%s*;")
                        if name then names[name] = true end
                    end
                end
            end
        end
        for file, _ in pairs(previous) do
            if not current[file] and not file:endswith(".cpp") then interfaces_changed = true end
        end
        for _, target in ipairs(project.ordertargets()) do
            if interfaces_changed then
                for _, artifact in ipairs(os.files(path.join(target:autogendir(), "rules/bmi/**"))) do
                    if names[path.basename(artifact)] then os.tryrm(artifact) end
                end
            end
            for _, file in ipairs(target:sourcefiles()) do
                if current[file] and (changed[file] or interfaces_changed) then
                    os.tryrm(target:objectfile(file))
                end
            end
        end
        inputs:set(signature, current)
        inputs:save()
    end)
rule_end()
