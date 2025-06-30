#pragma once

#include "mdb_internal.h"

// assert(3) variant in cursor context
#define mdb_cassert(mc, expr) mdb_assert0((mc)->mc_txn->mt_env, expr, #expr)
// assert(3) variant in transaction context
#define mdb_tassert(txn, expr) mdb_assert0((txn)->mt_env, expr, #expr)

#ifndef NDEBUG
#define mdb_assert0(env, expr, expr_txt)                                                                               \
    ((expr) ? (void)0 : mdb_assert_fail(env, expr_txt, __func__, __FILE__, __LINE__))
void ESECT mdb_assert_fail(MDB_env* env, const char* expr_txt, const char* func, const char* file, int line);
#else
#define mdb_assert0(env, expr, expr_txt) ((void)0)
#endif /* NDEBUG */

// Debug Macros
#ifndef MDB_DEBUG
//	Enable debug output.  Needs variable argument macros (a C99 feature).
//	Set this to 1 for copious tracing. Set to 2 to add dumps of all IDLs
//	read from and written to the database (used for free space management).
#define MDB_DEBUG 0
#endif

enum
{
    MDB_DBG_INFO = 1,
    MDB_DBG_TRACE = 2
};

#if MDB_DEBUG
extern int mdb_debug;
extern txnid_t mdb_debug_start;

//	Print a debug message with printf formatting.
//	Requires double parenthesis around 2 or more args.
#define DPRINTF(args) ((void)((mdb_debug & MDB_DBG_INFO) && DPRINTF0 args))
#define DPRINTF0(fmt, ...) fprintf(stderr, "%s:%d " fmt "\n", __func__, __LINE__, __VA_ARGS__)
#else
#define DPRINTF(args) ((void)0)
#endif
//	Print a debug string.
//	The string is printed literally, with no format processing.
#define DPUTS(arg) DPRINTF(("%s", arg))

// Debugging output value of a cursor DBI: Negative in a sub-cursor.
#define DDBI(mc) (((mc)->mc_flags & C_SUB) ? -(int)(mc)->mc_dbi : (int)(mc)->mc_dbi)

#if MDB_DEBUG
//	Key size which fits in a #DKBUF.
#define DKBUF_MAXKEYSIZE ((MDB_MAXKEYSIZE) > 0 ? (MDB_MAXKEYSIZE) : 511)
//	A key buffer.
//	This is used for printing a hex dump of a key's contents.
#define DKBUF char kbuf[(DKBUF_MAXKEYSIZE * 2) + 1]
//	A data value buffer.
//	This is used for printing a hex dump of a data value's contents.
#define DDBUF char dbuf[(DKBUF_MAXKEYSIZE * 2) + 1 + 2]
//	Display a key in hex.
//	Invoke a function to display a key in hex.
#define DKEY(x) mdb_dkey(x, kbuf)
auto mdb_dbg_pgno(MDB_page* mp) -> pgno_t;
auto mdb_dval(MDB_txn* txn, MDB_dbi dbi, MDB_val* data, char* buf) -> char*;
#else
#define DKBUF
#define DDBUF
#define DKEY(x) 0
#endif
