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
            io.writefile(generated, io.readfile(generated) .. '\n#include <QResource>\nvoid initializeTemplateUi() { Q_INIT_RESOURCE(template_ui); }\n')
            compiler.compile(generated, object, {target = target})
        end, {dependfile = target:dependfile(object), files = table.join(headers, jsonfiles, {path.join(os.projectdir(), "xmake/qt.lua")}),
            changed = target:is_rebuilt(), lastmtime = os.mtime(object)})
    end)
rule_end()
