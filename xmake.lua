target("lmdb")
    set_kind("static")
    add_files("mdb.c", "midl.c")
    add_defines("MDB_DEBUG=2")

target("mtest")
    set_kind("binary")
    add_files("mtest.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mtest2")
    set_kind("binary")
    add_files("mtest2.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mtest3")
    set_kind("binary")
    add_files("mtest3.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mtest4")
    set_kind("binary")
    add_files("mtest4.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mtest5")
    set_kind("binary")
    add_files("mtest5.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mtest6")
    set_kind("binary")
    add_files("mtest6.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mdb_copy")
    set_kind("binary")
    add_files("mdb_copy.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mdb_drop")
    set_kind("binary")
    add_files("mdb_drop.c", "getopt.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mdb_dump")
    set_kind("binary")
    add_files("mdb_dump.c", "getopt.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mdb_load")
    set_kind("binary")
    add_files("mdb_load.c", "getopt.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")

target("mdb_stat")
    set_kind("binary")
    add_files("mdb_stat.c", "getopt.c")
    add_deps("lmdb")
    if is_plat("windows") then
        add_syslinks("advapi32")
    end
    add_defines("MDB_DEBUG=2")
