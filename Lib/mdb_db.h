#pragma once

#include "mdb_internal.h"

// Information about a single database in the environment.
struct MDB_db
{
    uint32_t md_pad;           // padding for alignment
    uint16_t md_flags;         // mdb_dbi_open
    uint16_t md_depth;         // depth of this tree
    pgno_t md_branch_pages;    // number of internal pages
    pgno_t md_leaf_pages;      // number of leaf pages
    pgno_t md_overflow_pages;  // number of overflow pages
    mdb_size_t md_entries;     // number of data items
    pgno_t md_root;            // the root page of this tree
};

// Auxiliary DB info.
// The information here is mostly static/read-only. There is
// only a single copy of this record in the environment.
struct MDB_dbx
{
    MDB_val md_name;        // name of the database
    MDB_cmp_func* md_cmp;   // function for comparing keys
    MDB_cmp_func* md_dcmp;  // function for comparing data items
    MDB_rel_func* md_rel;   // user relocate function
    void* md_relctx;        // user-provided context for md_rel
};

// Check txn and dbi arguments to a function
#define TXN_DBI_EXIST(txn, dbi, validity) ((txn) && (dbi) < (txn)->mt_numdbs && ((txn)->mt_dbflags[dbi] & (validity)))

// Check for misused dbi handles
#define TXN_DBI_CHANGED(txn, dbi) ((txn)->mt_dbiseqs[dbi] != (txn)->mt_env->me_dbiseqs[dbi])

int mdb_drop0(MDB_cursor* mc, int subs);
