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
add_defines("MDB_DEBUG=1")
if is_plat("windows") then
    add_cxflags("/wd4146")
end

target("fiksstore")
    set_kind("static")
    add_files("Lib/midl.cpp",
              "Lib/mdb_hash.cpp",
              "Lib/mdb_page_io.cpp",
              "Lib/mdb_btree.cpp",
              "Lib/mdb_util.cpp",
              "Lib/mdb_compare.cpp",
              "Lib/mdb_env.cpp",
              "Lib/mdb_txn.cpp",
              "Lib/mdb_cursor.cpp",
              "Lib/mdb_db.cpp",
              "Lib/mdb_lock.cpp",
              "Lib/mdb_debug.cpp")
    add_includedirs("Lib/", {public = true})

local testdir1 = path.join(os.tmpdir(), "test1")
target("mtest")
    set_kind("binary")
    add_files("Tests/mtest.cpp")
    add_deps("fiksstore")
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
    add_deps("fiksstore")
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
    add_deps("fiksstore")
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



target("mdb_copy")
    set_kind("binary")
    add_files("Tools/mdb_copy.cpp")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end

target("mdb_drop")
    set_kind("binary")
    add_files("Tools/mdb_drop.cpp")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end

target("mdb_dump")
    set_kind("binary")
    add_files("Tools/mdb_dump.cpp")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end

target("mdb_load")
    set_kind("binary")
    add_files("Tools/mdb_load.cpp")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end

target("mdb_stat")
    set_kind("binary")
    add_files("Tools/mdb_stat.cpp")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end

-- Format target for code formatting with clang-format
target("format")
    set_kind("phony")
    on_run(function (target)
        -- Find all .h and .cpp files in the specified directories
        local source_dirs = {"Lib", "Tests", "Tools"}
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
        -- Find all .cpp files in the specified directories
        local source_dirs = {"Lib", "Tests", "Tools"}
        local file_patterns = {"*.cpp"}
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
            print("No source files found to lint")
            return
        end

        print("Running clang-tidy with --fix on project files...")

        local build_dir = "build"
        if not os.isdir(build_dir) then
            os.mkdir(build_dir)
        end

        -- Run clang-tidy with --fix. The -p . argument tells clang-tidy to use
        -- the compile_commands.json from the current directory. clang-tidy will
        -- lint all files found in the compilation database.
        local command_args = {"-p", ".", "--fix"}
        for _, file in ipairs(files) do
            table.insert(command_args, file)
        end
        local output, errors = os.iorunv("clang-tidy", command_args)

        local output_file = path.join(build_dir, "clang-tidy-output.txt")
        io.writefile(output_file, output or "")
        if errors then
            -- Append errors to the same file
            local f = io.open(output_file, "a")
            if f then
                f:write("\n--- ERRORS ---\n")
                f:write(errors)
                f:close()
            end
        end

        -- os.iorunv returns the output as the first value. If there was an error,
        -- the second return value is a string describing the error.
        -- We check for the presence of the error string to determine success.
        if not errors then
            print("\nCode linting completed successfully!")
            print("Applied automatic fixes where possible.")
            print("Output and diagnostics written to " .. output_file)
        else
            print("\nCode linting completed with errors.")
            print("Please check the output file for details: " .. output_file)
        end
    end)
