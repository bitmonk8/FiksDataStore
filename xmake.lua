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

target("lmdb")
    set_kind("static")
    add_files("mdb.c", "midl.c")
    add_defines("MDB_DEBUG=0")
    add_includedirs(".", {public = true})

target("mtest")
    set_kind("binary")
    add_files("Tests/mtest.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test1")

target("mtest2")
    set_kind("binary")
    add_files("Tests/mtest2.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test2")

target("mtest3")
    set_kind("binary")
    add_files("Tests/mtest3.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test3")

target("mtest4")
    set_kind("binary")
    add_files("Tests/mtest4.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test4")

target("mtest5")
    set_kind("binary")
    add_files("Tests/mtest5.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test5")

target("mtest6")
    set_kind("binary")
    add_files("Tests/mtest6.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
    add_tests("test6", {fail_outputs = "TEST FAILED: ", plain = true})

target("mdb_copy")
    set_kind("binary")
    add_files("Tools/mdb_copy.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")

target("mdb_drop")
    set_kind("binary")
    add_files("Tools/mdb_drop.c", "getopt.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")

target("mdb_dump")
    set_kind("binary")
    add_files("Tools/mdb_dump.c", "getopt.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")

target("mdb_load")
    set_kind("binary")
    add_files("Tools/mdb_load.c", "getopt.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")

target("mdb_stat")
    set_kind("binary")
    add_files("Tools/mdb_stat.c", "getopt.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=0")
