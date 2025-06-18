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

target("fiksstore")
    set_kind("static")
    add_files("Lib/mdb.c", 
              "Lib/midl.c",
              "Lib/mdb_hash.c",
              "Lib/mdb_page.c",
              "Lib/mdb_util.c",
              "Lib/mdb_compare.c",
              "Lib/mdb_env.c",
              "Lib/mdb_txn.c",
              "Lib/mdb_cursor.c",
              "Lib/mdb_db.c")
    add_defines("MDB_DEBUG=0")
    add_includedirs("Lib/", {public = true})

local testdir1 = path.join(os.tmpdir(), "test1")
target("mtest")
    set_kind("binary")
    add_files("Tests/mtest.cpp")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    set_languages("cxx20")
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
            os.rmdir(testdir1)
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
    add_defines("MDB_DEBUG=0")
    set_languages("cxx20")
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
            os.rmdir(testdir2)
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
    add_defines("MDB_DEBUG=0")
    set_languages("cxx20")
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

local testdir4 = path.join(os.tmpdir(), "test4")
target("mtest4")
    set_kind("binary")
    add_files("Tests/mtest4.c")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test4", {
        rundir = testdir4,
        fail_outputs = "TEST FAILED: ", 
        plain = true
    })
    before_test(function (target)
        os.mkdir(testdir4)
    end)
    after_test(function (target)
        if os.isdir(testdir4) then
            os.rmdir(testdir4)
        end
    end)    

local testdir5 = path.join(os.tmpdir(), "test5")
target("mtest5")
    set_kind("binary")
    add_files("Tests/mtest5.c")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test5", {
        rundir = testdir5,
        fail_outputs = "TEST FAILED: ", 
        plain = true
    })
    before_test(function (target)
        os.mkdir(testdir5)
    end)
    after_test(function (target)
        if os.isdir(testdir5) then
            os.rmdir(testdir5)
        end
    end)    

local testdir6 = path.join(os.tmpdir(), "test6")
target("mtest6")
    set_kind("binary")
    add_files("Tests/mtest6.c")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test6", {fail_outputs = "TEST FAILED: ", plain = true})
    add_tests("test6", {
        rundir = testdir6,
        fail_outputs = "TEST FAILED: ", 
        plain = true
    })
    before_test(function (target)
        os.mkdir(testdir6)
    end)
    after_test(function (target)
        if os.isdir(testdir6) then
            os.rmdir(testdir6)
        end
    end)    

target("mdb_copy")
    set_kind("binary")
    add_files("Tools/mdb_copy.c")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")

target("mdb_drop")
    set_kind("binary")
    add_files("Tools/mdb_drop.c", "Lib/getopt.c")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")

target("mdb_dump")
    set_kind("binary")
    add_files("Tools/mdb_dump.c", "Lib/getopt.c")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")

target("mdb_load")
    set_kind("binary")
    add_files("Tools/mdb_load.c", "Lib/getopt.c")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")

target("mdb_stat")
    set_kind("binary")
    add_files("Tools/mdb_stat.c", "Lib/getopt.c")
    add_deps("fiksstore")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
