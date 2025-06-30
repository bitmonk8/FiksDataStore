import("core.base.process")

function MyExecV(program, argv, opt)

    -- is not executable program file?
    opt = opt or {}
    local filename = tostring(program)
    if not os.isexec(program) then
        -- parse the filename and arguments, e.g. "xcrun -sdk macosx clang"
        local splitinfo = program:split("%s")
        filename = splitinfo[1]
        if #splitinfo > 1 then
            argv = table.join(table.slice(splitinfo, 2), argv)
        end
    end

    -- init open options
    local openopt = {
        envs = envs,
        stdin = opt.stdin,
        stdout = opt.stdout,
        stderr = opt.stderr,
        curdir = opt.curdir,
        detach = opt.detach,
        exclusive = opt.exclusive}

    -- open command
    local ok = -1
    local errors
    local proc = process.openv(filename, argv or {}, openopt)
    if proc ~= nil then
        -- wait process
        if not opt.detach then
            local waitok, status = proc:wait(opt.timeout or -1)
            if waitok > 0 then
                ok = status
            elseif waitok == 0 and opt.timeout then
                proc:kill()
                waitok, status = proc:wait(-1)
                if waitok > 0 then
                    ok = status
                end
                errors = "wait process timeout"
            end
        else
            ok = 0
        end

        -- close process
        proc:close()
    else
        -- cannot execute process
        return nil, os.strerror()
    end

    return ok, errors
end
