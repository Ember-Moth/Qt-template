rule("template.qml")
    add_deps("qt.env")
    add_orders("qt.moc", "template.qml")
    on_config(function(target)
        target:add("qt.moc.flags", "--output-json")
    end)
    on_build_file(function(target, sourcefile, opt)
        import("core.project.depend")
        import("core.tool.compiler")
        import("lib.detect.find_file")
        local qt = target:data("qt")
        local moc = target:data("qt.moc")
        local registrar = assert(find_file(is_host("windows") and "qmltyperegistrar.exe" or "qmltyperegistrar",
            {qt.libexecdir, qt.bindir}), "qmltyperegistrar not found")
        local directory = target:data("template.qml.directory")
        local generated = path.join(directory, "template_ui_qmltyperegistrations.cpp")
        local metatypes = path.join(directory, "metatypes.json")
        local object = target:objectfile(generated)
        table.insert(target:objectfiles(), object)
        local headers = table.unique(target:sourcebatches()["qt.moc"].sourcefiles)
        local jsonfiles = {}
        for _, header in ipairs(headers) do
            local output = target:autogenfile(path.join(path.directory(header), "moc_" .. path.basename(header) .. ".cpp"))
            table.insert(jsonfiles, output .. ".json")
        end
        depend.on_changed(function()
            os.vrunv(moc, table.join({"--collect-json", "-o", metatypes}, jsonfiles))
            local foreign = {}
            for _, name in ipairs({"core", "gui", "qml", "quick"}) do
                local file = path.join(qt.sdkdir, "metatypes", "qt6" .. name .. "_metatypes.json")
                if os.isfile(file) then table.insert(foreign, file) end
            end
            local args = {"--import-name=Template.Ui", "--major-version=1", "--minor-version=0",
                "--generate-qmltypes=" .. path.join(directory, "plugin.qmltypes"), "-o", generated}
            if #foreign > 0 then table.insert(args, "--foreign-types=" .. table.concat(foreign, ",")) end
            table.insert(args, metatypes)
            os.vrunv(registrar, args)
            -- Referencing both resources keeps the QML files and compiled units in the static library link.
            io.writefile(generated, io.readfile(generated) .. '\n#include <QResource>\nvoid initializeTemplateUi()\n{\n'
                .. '    Q_INIT_RESOURCE(template_ui);\n    Q_INIT_RESOURCE(qmlcache_template_ui);\n}\n')
            compiler.compile(generated, object, {target = target})
        end, {dependfile = target:dependfile(object), files = table.join(headers, jsonfiles, {path.join(os.projectdir(), "xmake/qt.lua")}),
            changed = target:is_rebuilt(), lastmtime = os.mtime(object)})

        -- Compile QML ahead of time like qt_add_qml_module: one C++ unit per file, plus a loader
        -- that hands those units to the engine for their qrc paths. Runs after plugin.qmltypes exists.
        local cachegen = assert(find_file(is_host("windows") and "qmlcachegen.exe" or "qmlcachegen",
            {qt.libexecdir, qt.bindir}), "qmlcachegen not found")
        local qrc = target:data("template.qml.qrc")
        local qmldir = path.join(directory, "qmldir")
        local qmltypes = path.join(directory, "plugin.qmltypes")
        local cachedir = path.join(target:autogendir(), "qmlcache")
        local function cachegen_file(output, inputs, args)
            local cacheobject = target:objectfile(output)
            table.insert(target:objectfiles(), cacheobject)
            depend.on_changed(function()
                os.vrunv(cachegen, table.join(args, {"-o", output}))
                compiler.compile(output, cacheobject, {target = target})
            end, {dependfile = target:dependfile(cacheobject),
                files = table.join(inputs, {cachegen, qrc, path.join(os.projectdir(), "xmake/qt.lua")}),
                changed = target:is_rebuilt(), lastmtime = os.mtime(cacheobject)})
        end
        os.mkdir(cachedir)
        local resources = {"--resource", qrc}
        for _, qml in ipairs(target:data("template.qml.files")) do
            table.insert(resources, qml.resource)
            cachegen_file(path.join(cachedir, path.basename(qml.source) .. "_qml.cpp"), {qml.source, qmldir, qmltypes},
                {"--bare", "--resource-path", qml.resource, "-I", path.directory(path.directory(directory)),
                 "-I", qt.qmldir, "-i", qmldir, "--resource", qrc, qml.source})
        end
        local list = path.join(cachedir, "template_ui_qml_loader_file_list.rsp")
        local content = table.concat(resources, "\n") .. "\n"
        if not os.isfile(list) or io.readfile(list) ~= content then
            io.writefile(list, content)
        end
        -- qmlcachegen writes a loader when the output name ends in qmlcache_loader.cpp.
        cachegen_file(path.join(cachedir, "template_ui_qmlcache_loader.cpp"), {list},
            {"--resource-name", "qmlcache_template_ui", "@" .. list})
    end)
rule_end()
