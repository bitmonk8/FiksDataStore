#pragma once

#include "internal.h"
#include "page.h"
#include "util.h"
#include "midl.h"

// Forward declarations to avoid circular dependencies.
struct MDB_cursor;
struct MDB_txn;
struct MDB_env;
struct MDB_val;

// @defgroup mdb_page_io Page I/O and Memory Management
//  @{
//  @brief Low-level page allocation, I/O, and management.
//

// @brief Allocate page(s) for a transaction.
//
//  This function allocates one or more pages from the freelist or by extending the database file.
//  @param[in] mc       A cursor handle identifying the transaction.
//  @param[in] num      The number of pages to allocate.
//  @param[out] mp      Address where the pointer to the new page will be stored.
//  @return 0 on success, a non-zero error code on failure.
//
auto mdb_page_alloc(MDB_cursor* mc, int num, MDB_page** mp) -> int;

// @brief Retrieve a page by its page number.
//
//  This function retrieves a specific page, handling dirty pages from the current or parent transactions.
//  @param[in] mc       A cursor handle identifying the transaction.
//  @param[in] pgno     The page number to retrieve.
//  @param[out] mp      Address where the pointer to the page will be stored.
//  @param[out] lvl     The inheritance level of the page (1=current txn, 0=mapped).
//  @return 0 on success, a non-zero error code on failure.
//
auto mdb_page_get(MDB_cursor* mc, pgno_t pgno, MDB_page** mp, int* lvl) -> int;

// @brief Flush dirty pages to disk.
//
//  Writes all dirty pages for a transaction to the database file.
//  @param[in] txn      The transaction handle.
//  @param[in] keep     Number of initial pages in dirty_list to keep dirty.
//  @return 0 on success, a non-zero error code on failure.
//
auto mdb_page_flush(MDB_txn* txn, int keep) -> int;

// @brief Restore a spilled page.
//
//  If a page was spilled, this brings it back into memory and marks it as dirty.
//  @param[in] txn      The transaction handle.
//  @param[in] mp       The page to unspill.
//  @param[out] ret     The writable page.
//  @return 0 on success, a non-zero error code on failure.
//
auto mdb_page_unspill(MDB_txn* txn, MDB_page* mp, MDB_page** ret) -> int;

// @brief Free a sequence of overflow pages.
//
//  @param[in] mc       A cursor handle identifying the transaction.
//  @param[in] mp       The first page of the overflow sequence to free.
//  @return 0 on success, a non-zero error code on failure.
//
auto mdb_ovpage_free(MDB_cursor* mc, MDB_page* mp) -> int;

// @brief Spill dirty pages to disk to free up memory.
//
//  @param[in] m0       A cursor handle for the transaction.
//  @param[in] key      The key being stored (for space estimation).
//  @param[in] data     The data being stored (for space estimation).
//  @return 0 on success, a non-zero error code on failure.
//
auto mdb_page_spill(MDB_cursor* m0, MDB_val* key, MDB_val* data) -> int;

// @brief Mark a page as dirty.
//
//  @param[in] txn      The transaction handle.
//  @param[in] mp       The page to mark as dirty.
//
void mdb_page_dirty(MDB_txn* txn, MDB_page* mp);

// @brief Allocate memory for a page structure.
//
//  @param[in] txn      The transaction handle.
//  @param[in] num      The number of pages to allocate memory for.
//  @return Pointer to the allocated memory, or nullptr on failure.
//
auto mdb_page_malloc(MDB_txn* txn, unsigned num) -> MDB_page*;

// @brief Free a clean page.
//
//  Saves single pages to a list for future reuse.
//  @param[in] env      The environment handle.
//  @param[in] mp       The page to free.
//
void mdb_page_free(MDB_env* env, MDB_page* mp);

// @brief Free a dirty page.
//
//  @param[in] env      The environment handle.
//  @param[in] dp       The dirty page to free.
//
void mdb_dpage_free(MDB_env* env, MDB_page* dp);

// @}