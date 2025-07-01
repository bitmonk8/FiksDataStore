// midl.h
// LMDB ID List header file.
//
// MIDL stands for "Memory ID List" and provides specialized data structures
// and operations for managing sorted arrays of IDs in the LMDB system.
//
// This file was originally part of back-bdb but has been
// modified for use in libmdb. Most of the macros defined
// in this file are unused, just left over from the original.
//
// This file is only used internally in libmdb and its definitions
// are not exposed publicly.
//
// Usage in LMDB Context
// This code is used internally by LMDB for:
// - Free page management: Tracking which database pages are available for reuse
// - Transaction management: Managing transaction IDs and their associated data
// - Index operations: Maintaining sorted lists of record IDs
// - Memory mapping: Associating page IDs with memory locations
//
// Performance Characteristics
// - Search operations: O(log n) using binary search
// - Insert operations: O(n) for maintaining sort order
// - Append operations: O(1) for adding to end
// - Sort operations: O(n log n) using hybrid quicksort/insertion sort
// - Merge operations: O(n + m) for combining sorted lists
//
// Memory Layout
// IDLs use a compact array layout where allocation size is stored at ids[-1]
// for efficient dynamic resizing with 25% overhead plus 256-element alignment.
// $OpenLDAP$
// This work is part of OpenLDAP Software <http://www.openldap.org/>.
//
// Copyright 2000-2021 The OpenLDAP Foundation.
// Portions Copyright 2001-2021 Howard Chu, Symas Corp.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted only as authorized by the OpenLDAP
// Public License.
//
// A copy of this license is available in the file LICENSE in the
// top-level directory of the distribution or, alternatively, at
// <http://www.OpenLDAP.org/license.html>.

#pragma once

#include "fds.h"

// LMDB Internals
//
// ID List Management
// A generic unsigned ID number. These were entryIDs in back-bdb.
// Preferably it should have the same size as a pointer.
using FDS_ID = fds_size_t;

// An IDL is an ID List, a sorted array of IDs. The first
// element of the array is a counter for how many actual
// IDs are in the list. In the original back-bdb code, IDLs are
// sorted in ascending order. For libmdb IDLs are sorted in
// descending order.
//
// Memory layout: [count][id1][id2]...[idN]
// - Element [0]: Contains count of actual IDs
// - Elements [1] to [count]: Contains sorted IDs in descending order
// - Used for managing page IDs, transaction IDs, and database identifiers
using FDS_IDL = FDS_ID*;

/* IDL sizes - likely should be even bigger
 *   limiting factors: sizeof(ID), thread stack size
 */
#ifndef FDS_IDL_LOGN
#define FDS_IDL_LOGN 16 /* DB_SIZE is 2^16, UM_SIZE is 2^17 */
#endif
#define FDS_IDL_DB_SIZE (1 << FDS_IDL_LOGN)
#define FDS_IDL_UM_SIZE (1 << (FDS_IDL_LOGN + 1))

#define FDS_IDL_DB_MAX (FDS_IDL_DB_SIZE - 1)
#define FDS_IDL_UM_MAX (FDS_IDL_UM_SIZE - 1)

#define FDS_IDL_SIZEOF(ids) (((ids)[0] + 1) * sizeof(FDS_ID))
#define FDS_IDL_IS_ZERO(ids) ((ids)[0] == 0)
#define FDS_IDL_CPY(dst, src) (memcpy(dst, src, FDS_IDL_SIZEOF(src)))
#define FDS_IDL_FIRST(ids) ((ids)[1])
#define FDS_IDL_LAST(ids) ((ids)[(ids)[0]])

// Current max length of an #fds_midl_alloc()ed IDL
#define FDS_IDL_ALLOCLEN(ids) ((ids)[-1])

// Append ID to IDL. The IDL must be big enough.
#define fds_midl_xappend(idl, id)                                                                                      \
    do                                                                                                                 \
    {                                                                                                                  \
        FDS_ID* xidl = (idl);                                                                                          \
        FDS_ID xlen = ++(xidl[0]);                                                                                     \
        xidl[xlen] = (id);                                                                                             \
    } while (0)

// Search for an ID in an IDL.
// Uses binary search for O(log n) performance.
// ids The IDL to search.
// id The ID to search for.
// The index of the first ID greater than or equal to id.
auto fds_midl_search(const FDS_IDL ids, FDS_ID id) -> unsigned;

// Allocate an IDL.
// Allocates memory for an IDL of the given size.
// IDL on success, NULL on failure.
auto fds_midl_alloc(int num) -> FDS_IDL;

// Free an IDL.
// ids The IDL to free.
void fds_midl_free(FDS_IDL ids);

// Shrink an IDL.
// Return the IDL to the default size if it has grown larger.
// idp Address of the IDL to shrink.
void fds_midl_shrink(FDS_IDL* idp);

// Make room for num additional elements in an IDL.
// idp Address of the IDL.
// num Number of elements to make room for.
// 0 on success, ENOMEM on failure.
auto fds_midl_need(FDS_IDL* idp, unsigned num) -> int;

// Append an ID onto an IDL.
// idp Address of the IDL to append to.
// id The ID to append.
// 0 on success, ENOMEM if the IDL is too large.
auto fds_midl_append(FDS_IDL* idp, FDS_ID id) -> int;

// Append an IDL onto an IDL.
// idp Address of the IDL to append to.
// app The IDL to append.
// 0 on success, ENOMEM if the IDL is too large.
auto fds_midl_append_list(FDS_IDL* idp, FDS_IDL app) -> int;

// Append an ID range onto an IDL.
// idp Address of the IDL to append to.
// id The lowest ID to append.
// n Number of IDs to append.
// 0 on success, ENOMEM if the IDL is too large.
auto fds_midl_append_range(FDS_IDL* idp, FDS_ID id, unsigned n) -> int;

// Merge an IDL onto an IDL. The destination IDL must be big enough.
// idl The IDL to merge into.
// merge The IDL to merge.
void fds_midl_xmerge(FDS_IDL idl, const FDS_IDL merge);

// Sort an IDL.
// Uses hybrid quicksort with insertion sort optimization for small arrays.
// ids The IDL to sort.
void fds_midl_sort(FDS_IDL ids);

// An ID2 is an ID/pointer pair.
//
struct FDS_ID2
{
    FDS_ID mid;  // The ID
    void* mptr;  // The pointer
};

// An ID2L is an ID2 List, a sorted array of ID2s.
// The first element's mid member is a count of how many actual
// elements are in the array. The mptr member of the first element is unused.
// The array is sorted in ascending order by mid.
//
// Memory layout: [count_entry][id2_1][id2_2]...[id2_N]
// - Element [0].mid: Contains count of actual ID2 pairs
// - Element [0].mptr: Unused
// - Elements [1] to [count]: Contains ID/pointer pairs sorted by ID in ascending order
// - Used for mapping IDs to memory locations or data structures
using FDS_ID2L = FDS_ID2*;

// Search for an ID in an ID2L.
// Uses binary search for O(log n) performance.
// ids The ID2L to search.
// id The ID to search for.
// The index of the first ID2 whose mid member is greater than or equal to id.
auto fds_mid2l_search(FDS_ID2L ids, FDS_ID id) -> unsigned;

// Insert an ID2 into a ID2L.
// ids The ID2L to insert into.
// id The ID2 to insert.
// 0 on success, -1 if the ID was already present in the ID2L.
auto fds_mid2l_insert(FDS_ID2L ids, FDS_ID2* id) -> int;

// Append an ID2 into a ID2L.
// ids The ID2L to append into.
// id The ID2 to append.
// 0 on success, -2 if the ID2L is too big.
auto fds_mid2l_append(FDS_ID2L ids, FDS_ID2* id) -> int;
