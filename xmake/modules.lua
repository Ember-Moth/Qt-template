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
        -- Keep a per-configuration input snapshot and invalidate only the project artifacts that
        -- reach a changed file through #include and import edges.
        local inputs = localcache.cache("template-modules")
        local previous = inputs:get(signature) or {}
        local current, sources, bykey, byname = {}, {}, {}, {}
        local function key(file) return path.unix(path.normalize(file)) end
        for _, root in ipairs({"src", "tests"}) do
            for _, extension in ipairs({"cppm", "cpp", "h"}) do
                for _, file in ipairs(os.files(root .. "/**." .. extension)) do
                    current[file] = hash.sha256(file)
                    table.insert(sources, file)
                    bykey[key(file)] = file
                    byname[path.filename(file)] = byname[path.filename(file)] or {}
                    table.insert(byname[path.filename(file)], file)
                end
            end
        end
        -- A removed header or interface leaves no edge to follow, so rebuild every project artifact.
        local everything = false
        for file, _ in pairs(previous) do
            if not current[file] and not file:endswith(".cpp") then everything = true end
        end

        local includers, importers, definitions = {}, {}, {}
        local function add_edge(edges, name, file)
            edges[name] = edges[name] or {}
            table.insert(edges[name], file)
        end
        for _, file in ipairs(sources) do
            local module
            for line in io.readfile(file):gmatch("[^\n]+") do
                local include = line:match('^%s*#%s*include%s*"([^"]+)"')
                if include then
                    local found
                    for _, base in ipairs({path.directory(file), "src", "tests"}) do
                        found = found or bykey[key(path.join(base, include))]
                    end
                    -- An unresolved project include conservatively matches every header with its name.
                    for _, header in ipairs(found and {found} or byname[path.filename(include)] or {}) do
                        add_edge(includers, header, file)
                    end
                end
                local exported = line:match("^%s*export%s+module%s+([%w_.:]+)%s*;")
                local implemented = line:match("^%s*module%s+([%w_.:]+)%s*;")
                module = exported or implemented or module
                if exported then definitions[file] = exported end
                -- An implementation partition also supplies a BMI consumed by other module units.
                if implemented and implemented:find(":", 1, true) then definitions[file] = implemented end
                if implemented then add_edge(importers, implemented, file) end
                local imported = line:match("^%s*export%s+import%s+([%w_.:]+)%s*;")
                    or line:match("^%s*import%s+([%w_.:]+)%s*;")
                if imported then
                    if imported:startswith(":") and module then imported = module:match("^[^:]+") .. imported end
                    add_edge(importers, imported, file)
                end
            end
        end

        -- A dirty file dirties its includers; a dirty interface dirties its module and importers.
        local dirty, modules, queue = {}, {}, {}
        local function mark(file)
            if not dirty[file] then
                dirty[file] = true
                table.insert(queue, file)
            end
        end
        for _, file in ipairs(sources) do
            if everything or current[file] ~= previous[file] then mark(file) end
        end
        while #queue > 0 do
            local file = table.remove(queue)
            for _, includer in ipairs(includers[file] or {}) do mark(includer) end
            local name = definitions[file]
            if name then
                modules[name] = true
                modules[(name:gsub(":", "-"))] = true
                for _, importer in ipairs(importers[name] or {}) do mark(importer) end
            end
        end
        for _, target in ipairs(project.ordertargets()) do
            for _, artifact in ipairs(os.files(path.join(target:autogendir(), "rules/bmi/**"))) do
                if modules[path.basename(artifact)] then os.tryrm(artifact) end
            end
            for _, file in ipairs(target:sourcefiles()) do
                if dirty[file] then os.tryrm(target:objectfile(file)) end
            end
        end
        inputs:set(signature, current)
        inputs:save()
    end)
rule_end()
