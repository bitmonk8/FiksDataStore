#pragma once

#ifndef MDB_BTREE_H
#define MDB_BTREE_H

#include "mdb_cursor.h"
#include "mdb_internal.h"
#include "mdb_page.h"
#include "mdb_page_io.h"

// Page search flags
enum
{
    MDB_PS_MODIFY = 1,
    MDB_PS_ROOTONLY = 2,
    MDB_PS_FIRST = 4,
    MDB_PS_LAST = 8
};

// Split flags
enum
{
    MDB_SPLIT_REPLACE = 0x01
};

#define NODE_ADD_FLAGS                                                                                                 \
    (static_cast<unsigned>(F_SUBDATA) | static_cast<unsigned>(MDB_RESERVE) | static_cast<unsigned>(MDB_APPEND))

// B+ Tree Logic Module Functions

// @brief Search for a page in the database.
//  This is the public interface for page search.
//  @param[in] mc The cursor to operate on.
//  @param[in] key The key to search for.
//  @param[in] flags Flags for the search.
//  @return 0 on success, non-zero on failure.
//
auto mdb_page_search(MDB_cursor* mc, MDB_val* key, int flags) -> int;

// @brief Search for a page, beginning from the current root.
//  This is the main portion of #mdb_page_search().
//  @param[in] mc The cursor to operate on.
//  @param[in] key The key to search for.
//  @param[in] modify True if the search is for a modify operation.
//  @return 0 on success, non-zero on failure.
//
auto mdb_page_search_root(MDB_cursor* mc, MDB_val* key, int modify) -> int;

// @brief Search for the lowest key in the current subtree.
//  @param[in] mc The cursor to operate on.
//  @return 0 on success, non-zero on failure.
//
auto mdb_page_search_lowest(MDB_cursor* mc) -> int;

// @brief Search for key within a page.
//  @param[in] mc The cursor to operate on.
//  @param[in] key The key to search for.
//  @param[out] exactp Undefined on error. On success, points to 1 if exact match, 0 otherwise.
//  @return Pointer to the node if found, NULL otherwise.
//
auto mdb_node_search(MDB_cursor* mc, MDB_val* key, int* exactp) -> MDB_node*;

// @brief Add a node to a page.
//  @param[in] mc The cursor to operate on.
//  @param[in] indx The index on the page where the new node should be added.
//  @param[in] key The key of the new node.
//  @param[in] data The data of the new node.
//  @param[in] pgno The page number, if the new node is a branch node.
//  @param[in] flags Flags for the node.
//  @return 0 on success, non-zero on failure.
//
auto mdb_node_add(MDB_cursor* mc, indx_t indx, MDB_val* key, MDB_val* data, pgno_t pgno, unsigned int flags) -> int;

// @brief Delete a node from a page.
//  @param[in] mc The cursor to operate on.
//  @param[in] ksize The size of the key to delete.
//
void mdb_node_del(MDB_cursor* mc, int ksize);

// @brief Shrink a node in a page.
//  @param[in] mp The page containing the node.
//  @param[in] indx The index of the node to shrink.
//
void mdb_node_shrink(MDB_page* mp, indx_t indx);

// @brief Move a node from one page to another.
//  This is part of rebalancing.
//  @param[in] csrc The source cursor.
//  @param[in] cdst The destination cursor.
//  @param[in] fromleft Non-zero if the node is from the left sibling.
//  @return 0 on success, non-zero on failure.
//
auto mdb_node_move(MDB_cursor* csrc, MDB_cursor* cdst, int fromleft) -> int;

// @brief Read the data of a node.
//  @param[in] mc The cursor for context.
//  @param[in] leaf The node to read from.
//  @param[out] data The MDB_val to store the data.
//  @return 0 on success, non-zero on failure.
//
auto mdb_node_read(MDB_cursor* mc, MDB_node* leaf, MDB_val* data) -> int;

// @brief Split a page.
//  @param[in] mc The cursor to operate on. The page to be split is at the top of the cursor stack.
//  @param[in] newkey The key that prompted the split.
//  @param[in] newdata The data that prompted the split.
//  @param[in] newpgno The page number, if the new item is a branch.
//  @param[in] nflags Flags for the new node.
//  @return 0 on success, non-zero on failure.
//
auto mdb_page_split(MDB_cursor* mc, MDB_val* newkey, MDB_val* newdata, pgno_t newpgno, unsigned int nflags) -> int;

// @brief Merge one page into another.
//  The page pointed to by \b csrc will be merged into \b cdst and then \b csrc's page will be freed.
//  @param[in] csrc Cursor pointing to the source page.
//  @param[in] cdst Cursor pointing to the destination page.
//  @return 0 on success, non-zero on failure.
//
auto mdb_page_merge(MDB_cursor* csrc, MDB_cursor* cdst) -> int;

// @brief Rebalance the tree after a delete.
//  @param[in] mc The cursor to operate on. The page needing rebalancing is at the top of the cursor stack.
//  @return 0 on success, non-zero on failure.
//
auto mdb_rebalance(MDB_cursor* mc) -> int;

// @brief Touch a page: make it dirty and re-insert into tree.
//  @param[in] mc The cursor to operate on. The page is at the top of the cursor stack.
//  @return 0 on success, non-zero on failure.
//
auto mdb_page_touch(MDB_cursor* mc) -> int;

// @brief Allocate and initialize a new page.
//  This function is used to allocate a new page with a specific type
//  (leaf, branch, overflow). It uses mdb_page_alloc from mdb_page_io for the actual allocation.
//  @param[in] mc Cursor holding transaction and environment.
//  @param[in] flags Flags for the new page (P_BRANCH, P_LEAF, P_OVERFLOW).
//  @param[in] num Number of pages to allocate (usually 1).
//  @param[out] mp Pointer to the new page.
//  @return 0 on success, non-zero on failure.
//
auto mdb_page_new(MDB_cursor* mc, uint32_t flags, int num, MDB_page** mp) -> int;

// @brief Calculate the size of a leaf node.
//  @param[in] env The environment handle.
//  @param[in] key The key.
//  @param[in] data The data.
//  @return The size of the node.
//
auto mdb_leaf_size(MDB_env* env, MDB_val* key, MDB_val* data) -> size_t;

// @brief Calculate the size of a branch node.
//  @param[in] env The environment handle.
//  @param[in] key The key.
//  @return The size of the node.
//
auto mdb_branch_size(MDB_env* env, MDB_val* key) -> size_t;

// @brief Copy the contents of a page.
//  @param[out] dst The destination page.
//  @param[in] src The source page.
//  @param[in] psize The size of the pages.
//
void mdb_page_copy(MDB_page* dst, MDB_page* src, unsigned int psize);

#endif  // MDB_BTREE_H