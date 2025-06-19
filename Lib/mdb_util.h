#pragma once

#include "midl.h"

/** @defgroup util Utility Macros
 *	General-purpose utility macros for LMDB.
 *	@{
 */

	/** Test if the flags \b f are set in a flag word \b w. */
#define F_ISSET(w, f)	 (((w) & (f)) == (f))

	/** Round \b n up to an even number. */
#define EVEN(n)		(((n) + 1U) & -2) /* sign-extending -2 to match n+1U */

	/** Least significant 1-bit of \b n.  n must be of an unsigned type. */
#define LOW_BIT(n)		((n) & (-(n)))

	/** (log2(\b p2) % \b n), for p2 = power of 2 and 0 < n < 8. */
#define LOG2_MOD(p2, n)	(7 - 86 / ((p2) % ((1U<<(n))-1) + 11))
	/* Explanation: Let p2 = 2**(n*y + x), x<n and M = (1U<<n)-1. Now p2 =
	 * (M+1)**y * 2**x = 2**x (mod M). Finally "/" "happens" to return 7-x.
	 */

	/** Should be alignment of \b type. Ensure it is a power of 2. */
#define ALIGNOF2(type) \
	LOW_BIT(offsetof(struct { char ch_; type align_; }, align_))

	/** Enough space for 2^32 nodes with minimum of 2 keys per node. I.e., plenty.
	 * At 4 keys per node, enough for 2^64 nodes, so there's probably no need to
	 * raise this on a 64 bit machine.
	 */
#define CURSOR_STACK		 32

	/** max number of pages to commit in one writev() call */
#define MDB_COMMIT_PAGES	 64
#if defined(IOV_MAX) && IOV_MAX < MDB_COMMIT_PAGES
#undef MDB_COMMIT_PAGES
#define MDB_COMMIT_PAGES	IOV_MAX
#endif

	/** max bytes to write in one call */
#define MAX_WRITE		0x40000000U

	/** A page number in the database.
	 *	Note that 64 bit page numbers are overkill, since pages themselves
	 *	already represent 12-13 bits of addressable memory, and the OS will
	 *	always limit applications to a maximum of 63 bits of address space.
	 *
	 *	@note In the #MDB_node structure, we only store 48 bits of this value,
	 *	which thus limits us to only 60 bits of addressable data.
	 */
typedef MDB_ID	pgno_t;

	/** A transaction ID.
	 *	See struct MDB_txn.mt_txnid for details.
	 */
typedef MDB_ID	txnid_t;

	/**	Used for offsets within a single page.
	 *	Since memory pages are typically 4 or 8KB in size, 12-13 bits,
	 *	this is plenty.
	 */
typedef uint16_t	 indx_t;

	/** Platform-specific string duplication function */
#ifdef _WIN32
	   #define mdb_strdup _strdup
#else
	   #define mdb_strdup strdup
#endif

/** @} */