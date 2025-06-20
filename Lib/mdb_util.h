#pragma once

#include "mdb_internal.h"
#include "midl.h"

// @defgroup util Utility Macros
// General-purpose utility macros for LMDB.
// @{
//

	/** Test if the flags \b f are set in a flag word \b w. */
#define F_ISSET(w, f)	 (((w) & (f)) == (f))

	/** Round \b n up to an even number. */
#define EVEN(n)		(((n) + 1U) & -2) /* sign-extending -2 to match n+1U */

	/** Least significant 1-bit of \b n.  n must be of an unsigned type. */
#define LOW_BIT(n)		((n) & (-(n)))

	/** (log2(\b p2) % \b n), for p2 = power of 2 and 0 < n < 8. */
#define LOG2_MOD(p2, n)	(7 - 86 / ((p2) % ((1U<<(n))-1) + 11))
	// Explanation: Let p2 = 2**(n*y + x), x<n and M = (1U<<n)-1. Now p2 =
	// (M+1)**y * 2**x = 2**x (mod M). Finally "/" "happens" to return 7-x.
	//

	/** Should be alignment of \b type. Ensure it is a power of 2. */
#define ALIGNOF2(type) \
	LOW_BIT(offsetof(struct { char ch_; type align_; }, align_))

	/** max number of pages to commit in one writev() call */
#define MDB_COMMIT_PAGES	 64
#if defined(IOV_MAX) && IOV_MAX < MDB_COMMIT_PAGES
#undef MDB_COMMIT_PAGES
#define MDB_COMMIT_PAGES	IOV_MAX
#endif

	/** Platform-specific string duplication function */
#ifdef _WIN32
	   #define mdb_strdup _strdup
#else
	   #define mdb_strdup strdup
#endif

/** @} */