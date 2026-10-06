function main(name, version, url, checksum)
    import("net.http")
    import("utils.archive")
    local root = path.join(os.projectdir(), "build", "_deps", name .. "-" .. version)
    if not os.isdir(root) then
        local tarball = root .. ".tar.gz"
        os.mkdir(path.directory(root))
        if not os.isfile(tarball) then
            http.download(url, tarball)
        end
        assert(hash.sha256(tarball) == checksum, "Checksum mismatch for " .. name)
        local extracted = root .. "-extract"
        archive.extract(tarball, extracted)
        local directories = os.dirs(path.join(extracted, "*"))
        assert(#directories == 1, "Expected one source directory for " .. name)
        os.mv(directories[1], root)
        os.tryrm(extracted)
    end
    return root
end
