rule("template.ide")
    set_kind("project")
    after_build(function()
        import("core.project.project")
        import("core.base.task")
        import("core.tool.compiler")
        import("core.base.json")
        local support = import("support", {rootdir = path.join(os.programdir(), "rules/c++/modules")})
        local target = project.target("template_gui") or project.target("template_core")
        task.run("project", {kind = "compile_commands", outputdir = ".", lsp = "clangd"})
        local entries = json.loadfile("compile_commands.json")
        local filtered, seen = {}, {}
        for _, entry in ipairs(entries) do
            local program = path.basename(entry.arguments[1]):lower()
            local file = path.absolute(entry.file, entry.directory)
            if program ~= "moc" and program ~= "moc.exe" and not file:endswith(".cppm") and not seen[file] then
                table.insert(filtered, entry)
                seen[file] = true
            end
        end
        local module_files = {}
        for _, owner in ipairs(project.ordertargets()) do
            for _, file in ipairs(owner:sourcefiles()) do
                if file:endswith(".cppm") then
                    table.insert(module_files, {file = file, target = owner})
                end
            end
        end
        for _, file in ipairs(support.get_stdmodules(target)) do
            table.insert(module_files, {file = file, target = target})
        end
        for _, module in ipairs(module_files) do
            local file, target = module.file, module.target
            if not seen[path.absolute(file)] then
                local program, arguments = compiler.compargv(file, target:objectfile(file),
                    {target = target, sourcekind = "cxx", rawargs = true})
                table.insert(filtered, {directory = os.projectdir(), file = file,
                    arguments = table.join(program, arguments)})
            end
        end
        json.savefile("compile_commands.json", filtered)
    end)
rule_end()
