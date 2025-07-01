#pragma once

#include "internal.h"

// assert(3) variant in cursor context
#define fds_cassert(mc, expr) fds_assert0((mc)->mc_txn->mt_env, expr, #expr)
// assert(3) variant in transaction context
#define fds_tassert(txn, expr) fds_assert0((txn)->mt_env, expr, #expr)

#ifndef NDEBUG
#define fds_assert0(env, expr, expr_txt)                                                                               \
    ((expr) ? (void)0 : fds_assert_fail(env, expr_txt, __func__, __FILE__, __LINE__))
void ESECT fds_assert_fail(FDS_env* env, const char* expr_txt, const char* func, const char* file, int line);
#else
#define fds_assert0(env, expr, expr_txt) ((void)0)
#endif /* NDEBUG */

// Debug Macros
#ifndef FDS_DEBUG
//	Enable debug output.  Needs variable argument macros (a C99 feature).
//	Set this to 1 for copious tracing. Set to 2 to add dumps of all IDLs
//	read from and written to the database (used for free space management).
#define FDS_DEBUG 0
#endif

enum
{
    FDS_DBG_INFO = 1,
    FDS_DBG_TRACE = 2
};

#if FDS_DEBUG
extern int fds_debug;
extern txnid_t fds_debug_start;

//	Print a debug message with printf formatting.
//	Requires double parenthesis around 2 or more args.
#define DPRINTF(args) ((void)((fds_debug & FDS_DBG_INFO) && DPRINTF0 args))
#define DPRINTF0(fmt, ...) fprintf(stderr, "%s:%d " fmt "\n", __func__, __LINE__, __VA_ARGS__)
#else
#define DPRINTF(args) ((void)0)
#endif
//	Print a debug string.
//	The string is printed literally, with no format processing.
#define DPUTS(arg) DPRINTF(("%s", arg))

// Debugging output value of a cursor DBI: Negative in a sub-cursor.
#define DDBI(mc) ((int)(mc)->mc_dbi)

#if FDS_DEBUG
//	Key size which fits in a #DKBUF.
#define DKBUF_MAXKEYSIZE ((FDS_MAXKEYSIZE) > 0 ? (FDS_MAXKEYSIZE) : 511)
//	A key buffer.
//	This is used for printing a hex dump of a key's contents.
#define DKBUF char kbuf[(DKBUF_MAXKEYSIZE * 2) + 1]
//	A data value buffer.
//	This is used for printing a hex dump of a data value's contents.
#define DDBUF char dbuf[(DKBUF_MAXKEYSIZE * 2) + 1 + 2]
//	Display a key in hex.
//	Invoke a function to display a key in hex.
#define DKEY(x) fds_dkey(x, kbuf)
auto fds_dbg_pgno(FDS_page* mp) -> pgno_t;
auto fds_dval(FDS_txn* txn, FDS_dbi dbi, FDS_val* data, char* buf) -> char*;
#else
#define DKBUF
#define DDBUF
#define DKEY(x) 0
#endif
