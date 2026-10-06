function generate(target)
    local function write_if_changed(file, content)
        if not os.isfile(file) or io.readfile(file) ~= content then
            io.writefile(file, content)
        end
    end
    for _, pattern in ipairs({"src/viewmodels/**.h", "src/app/**.h", "src/ui/*.h"}) do
        for _, header in ipairs(os.files(pattern)) do
            target:add("includedirs", path.directory(header))
        end
    end
    local directory = path.join(target:autogendir(), "qml", "Template", "Ui")
    os.mkdir(directory)
    local entries = {"module Template.Ui", "typeinfo plugin.qmltypes", "prefer :/qt/qml/Template/Ui/"}
    local resources = {'<RCC><qresource prefix="/qt/qml/Template/Ui">'}
    for _, file in ipairs(os.files("src/ui/**.qml")) do
        local name = path.basename(file)
        write_if_changed(path.join(directory, name .. ".qml"), io.readfile(file))
        local singleton = (io.readfile(file) or ""):find("pragma Singleton", 1, true)
        table.insert(entries, (singleton and "singleton " or "") .. name .. " 1.0 " .. name .. ".qml")
        table.insert(resources, string.format('<file alias="%s.qml">%s</file>', name, path.absolute(file)))
    end
    local qmldir = path.join(directory, "qmldir")
    write_if_changed(qmldir, table.concat(entries, "\n") .. "\n")
    table.insert(resources, '<file alias="qmldir">' .. path.absolute(qmldir) .. '</file>')
    table.insert(resources, "</qresource></RCC>")
    local qrc = path.join(target:autogendir(), "template_ui.qrc")
    write_if_changed(qrc, table.concat(resources, "\n"))
    target:add("files", qrc)
    target:data_set("template.qml.directory", directory)
end
