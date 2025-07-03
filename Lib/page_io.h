#pragma once

#include "internal.h"
#include "midl.h"
#include "page.h"
#include "util.h"

// Forward declarations to avoid circular dependencies.
// DESIGN_VIOLATION: Forward-declarable types from `Lib/*.h` must be declared in `internal.h`, not defined there.
struct FDS_cursor;
// DESIGN_VIOLATION: Forward-declarable types from `Lib/*.h` must be declared in `internal.h`, not defined there.
struct FDS_txn;
// DESIGN_VIOLATION: Forward-declarable types from `Lib/*.h` must be declared in `internal.h`, not defined there.
struct FDS_env;
// DESIGN_VIOLATION: Forward-declarable types from `Lib/*.h` must be declared in `internal.h`, not defined there.
struct FDS_val;

// @defgroup fds_page_io Page I/O and Memory Management
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
auto fds_page_alloc(FDS_cursor* mc, int num, FDS_page** mp) -> int;

// @brief Retrieve a page by its page number.
//
//  This function retrieves a specific page, handling dirty pages from the current or parent transactions.
//  @param[in] mc       A cursor handle identifying the transaction.
//  @param[in] pgno     The page number to retrieve.
//  @param[out] mp      Address where the pointer to the page will be stored.
//  @param[out] lvl     The inheritance level of the page (1=current txn, 0=mapped).
//  @return 0 on success, a non-zero error code on failure.
//
auto fds_page_get(FDS_cursor* mc, pgno_t pgno, FDS_page** mp, int* lvl) -> int;

// @brief Flush dirty pages to disk.
//
//  Writes all dirty pages for a transaction to the database file.
//  @param[in] txn      The transaction handle.
//  @param[in] keep     Number of initial pages in dirty_list to keep dirty.
//  @return 0 on success, a non-zero error code on failure.
//
auto fds_page_flush(FDS_txn* txn, int keep) -> int;

// @brief Restore a spilled page.
//
//  If a page was spilled, this brings it back into memory and marks it as dirty.
//  @param[in] txn      The transaction handle.
//  @param[in] mp       The page to unspill.
//  @param[out] ret     The writable page.
//  @return 0 on success, a non-zero error code on failure.
//
auto fds_page_unspill(FDS_txn* txn, FDS_page* mp, FDS_page** ret) -> int;

// @brief Free a sequence of overflow pages.
//
//  @param[in] mc       A cursor handle identifying the transaction.
//  @param[in] mp       The first page of the overflow sequence to free.
//  @return 0 on success, a non-zero error code on failure.
//
auto fds_ovpage_free(FDS_cursor* mc, FDS_page* mp) -> int;

// @brief Spill dirty pages to disk to free up memory.
//
//  @param[in] m0       A cursor handle for the transaction.
//  @param[in] key      The key being stored (for space estimation).
//  @param[in] data     The data being stored (for space estimation).
//  @return 0 on success, a non-zero error code on failure.
//
auto fds_page_spill(FDS_cursor* m0, FDS_val* key, FDS_val* data) -> int;

// @brief Mark a page as dirty.
//
//  @param[in] txn      The transaction handle.
//  @param[in] mp       The page to mark as dirty.
//
void fds_page_dirty(FDS_txn* txn, FDS_page* mp);

// @brief Allocate memory for a page structure.
//
//  @param[in] txn      The transaction handle.
//  @param[in] num      The number of pages to allocate memory for.
//  @return Pointer to the allocated memory, or nullptr on failure.
//
auto fds_page_malloc(FDS_txn* txn, unsigned num) -> FDS_page*;

// @brief Free a clean page.
//
//  Saves single pages to a list for future reuse.
//  @param[in] env      The environment handle.
//  @param[in] mp       The page to free.
//
void fds_page_free(FDS_env* env, FDS_page* mp);

// @brief Free a dirty page.
//
//  @param[in] env      The environment handle.
//  @param[in] dp       The dirty page to free.
//
void fds_dpage_free(FDS_env* env, FDS_page* dp);

// @}