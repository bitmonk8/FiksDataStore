add_moduledirs("xmake_modules")

add_rules("mode.debug")
set_defaultmode("debug")

if is_mode("debug") then
    set_symbols("debug")
    set_optimize("none")
end

if is_mode("release") then
    set_symbols("hidden")
    set_optimize("fastest")
    set_strip("all")
end

set_languages("cxx20")
set_warnings("error")
add_defines("FDS_DEBUG=1")
function apply_common_flags()
    if is_plat("windows") then
        add_cxxflags("/wd4146")
    end
end

target("fiksdatastore")
    set_kind("static")
    add_files("Lib/midl.cpp",
              "Lib/hash.cpp",
              "Lib/page_io.cpp",
              "Lib/btree.cpp",
              "Lib/util.cpp",
              "Lib/compare.cpp",
              "Lib/env.cpp",
              "Lib/txn.cpp",
              "Lib/cursor.cpp",
              "Lib/db.cpp",
              "Lib/lock.cpp",
              "Lib/debug.cpp")
    add_includedirs("Lib/", {public = true})
    apply_common_flags()

local testdir1 = path.join(os.tmpdir(), "test1")
target("mtest")
    set_kind("binary")
    add_files("Tests/mtest.cpp")
    add_deps("fiksdatastore")
    apply_common_flags()
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_tests("test1", {
        rundir = testdir1,
        fail_outputs = "TEST FAILED: ",
        plain = true
    })
    before_test(function (target)
        os.mkdir(testdir1)
    end)
    after_test(function (target)
        if os.isdir(testdir1) then
            os.rm(testdir1)
        end
    end)

local testdir2 = path.join(os.tmpdir(), "test2")
target("mtest2")
    set_kind("binary")
    add_files("Tests/mtest2.cpp")
    add_deps("fiksdatastore")
    apply_common_flags()
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_tests("test2", {
        rundir = testdir2,
        fail_outputs = "TEST FAILED: ", 
        plain = true
    })
    before_test(function (target)
        os.mkdir(testdir2)
    end)
    after_test(function (target)
        if os.isdir(testdir2) then
            os.rm(testdir2)
        end
    end)


local testdir3 = path.join(os.tmpdir(), "test3")
target("mtest3")
    set_kind("binary")
    add_files("Tests/mtest3.cpp")
    add_deps("fiksdatastore")
    apply_common_flags()
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_tests("test3", {
        rundir = testdir3,
        fail_outputs = "TEST FAILED: ",
        plain = true
    })
    before_test(function (target)
        os.mkdir(testdir3)
    end)
    after_test(function (target)
        if os.isdir(testdir3) then
            os.rmdir(testdir3)
        end
    end)




-- Format target for code formatting with clang-format
target("format")
    set_kind("phony")
    on_run(function (target)
        -- Find all .h and .cpp files in the specified directories
        local source_dirs = {"Lib", "Tests"}
        local file_patterns = {"*.h", "*.cpp"}
        local files = {}
        
        for _, dir in ipairs(source_dirs) do
            if os.isdir(dir) then
                for _, pattern in ipairs(file_patterns) do
                    local found_files = os.files(path.join(dir, pattern))
                    for _, file in ipairs(found_files) do
                        table.insert(files, file)
                    end
                end
            end
        end
        
        if #files == 0 then
            print("No source files found to format")
            return
        end
        
        print("Formatting " .. #files .. " source files...")
        
        -- Run clang-format on all found files
        local clang_format_cmd = "clang-format -i"
        for _, file in ipairs(files) do
            clang_format_cmd = clang_format_cmd .. " " .. file
        end
        
        -- Execute the formatting command
        local ok, errors = os.iorunv("clang-format", table.join({"-i"}, files))
        if not ok then
            print("Error running clang-format: " .. (errors or "unknown error"))
            print("Make sure clang-format is installed and available in PATH")
            os.exit(1)
        end
        
        print("Code formatting completed successfully!")
        print("Formatted files in directories: " .. table.concat(source_dirs, ", "))
    end)

-- Lint target for code linting with clang-tidy
target("lint")
    set_kind("phony")
    on_run(function (target)
        import("core.base.option")
        import("execv")
        local args = option.get("arguments")
        local file_to_lint = args and args[1]

        local files = {}
        if file_to_lint and file_to_lint ~= "" then
            if os.isfile(file_to_lint) then
                table.insert(files, file_to_lint)
                print("Running clang-tidy on " .. file_to_lint .. "...")
            else
                print("Error: File not found at: " .. file_to_lint)
                os.exit(1)
            end
        else
            print("Running clang-tidy on all project files...")
            local source_dirs = {"Lib", "Tests"}
            local file_patterns = {"*.cpp"}
            for _, dir in ipairs(source_dirs) do
                if os.isdir(dir) then
                    for _, pattern in ipairs(file_patterns) do
                        local found_files = os.files(path.join(dir, pattern))
                        for _, file in ipairs(found_files) do
                            table.insert(files, file)
                        end
                    end
                end
            end
        end

        if #files == 0 then
            print("No source files found to lint")
            return
        end

        if not os.isdir("build") then
            os.mkdir("build")
        end

        os.run("xmake project -k compile_commands build")

        local db_path = "build/compile_commands.json"
        local f_read = io.open(db_path, "r")
        if f_read then
            local content = f_read:read("*a")
            f_read:close()

            content = content:gsub("%%s?-Wno%%-unused%%-command%%-line%%-argument", "")
            content = content:gsub("%%s?-Werror", "")

            local f_write = io.open(db_path, "w")
            if f_write then
                f_write:write(content)
                f_write:close()
                print("Temporarily sanitized compile_commands.json for linting.")
            end
        end

        local fix_command_args = {"-p", "build", "--header-filter=.*", "--fix", "--fix-errors", "--quiet"}
        for _, file in ipairs(files) do
            table.insert(fix_command_args, file)
        end

        local nullLogFile = os.tmpfile()
        execv.MyExecV("clang-tidy", fix_command_args, {stdout = nullLogFile, stderr = nullLogFile})
        os.rm(nullLogFile)

        local command_args = {"-p", "build", "--header-filter=.*"}
        for _, file in ipairs(files) do
            table.insert(command_args, file)
        end

        local args_string = os.args(command_args)
        local cmdline = "clang-tidy " .. args_string
        print(cmdline)

        local out_file    = path.join("build", "lint_output.txt")
        local ok, errors = execv.MyExecV("clang-tidy", command_args, {stdout = out_file, stderr = out_file})

        local lint_output = io.readfile(out_file)
        if lint_output and #lint_output > 0 then
            print(lint_output)
        end

        os.run("xmake project -k compile_commands build")

        if ok == 0 then
            print("\nCode linting completed successfully!")
        else
            print("\nCode linting completed with errors.")
        end
    end)
