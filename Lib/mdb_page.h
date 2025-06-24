#pragma once

#include "mdb_internal.h"
#include "mdb_util.h"
#include "midl.h"

// Common header for all page types. The page type depends on mp_flags.
struct MDB_page
{
#define mp_pgno mp_p.p_pgno
#define mp_next mp_p.p_next
    union
    {
        pgno_t p_pgno;            // page number
        struct MDB_page* p_next;  // for in-memory list of freed pages
    } mp_p;
    uint16_t mp_pad;    // key size if this is a LEAF2 page
    uint16_t mp_flags;  // mdb_page
#define mp_lower mp_pb.pb.pb_lower
#define mp_upper mp_pb.pb.pb_upper
#define mp_pages mp_pb.pb_pages
    union
    {
        struct
        {
            indx_t pb_lower;  // lower bound of free space
            indx_t pb_upper;  // upper bound of free space
        } pb;
        uint32_t pb_pages;  // number of overflow pages
    } mp_pb;
    indx_t mp_ptrs[0];  // dynamic size
};

// Alternate page header, for 2-byte aligned access
struct MDB_page2
{
    uint16_t mp2_p[sizeof(pgno_t) / 2];
    uint16_t mp2_pad;
    uint16_t mp2_flags;
    indx_t mp2_lower;
    indx_t mp2_upper;
    indx_t mp2_ptrs[0];
};

#define MP_PGNO(p) (reinterpret_cast<MDB_page2*>(p)->mp2_p)
#define MP_PAD(p) (reinterpret_cast<MDB_page2*>(p)->mp2_pad)
#define MP_FLAGS(p) (reinterpret_cast<MDB_page2*>(p)->mp2_flags)
#define MP_LOWER(p) (reinterpret_cast<MDB_page2*>(p)->mp2_lower)
#define MP_UPPER(p) (reinterpret_cast<MDB_page2*>(p)->mp2_upper)
#define MP_PTRS(p) (reinterpret_cast<MDB_page2*>(p)->mp2_ptrs)

// Size of the page header, excluding dynamic data at the end
#define PAGEHDRSZ ((unsigned)offsetof(MDB_page, mp_ptrs))

// Address of first usable data byte in a page, after the header
#define METADATA(p) (reinterpret_cast<void*>(reinterpret_cast<char*>(p) + PAGEHDRSZ))

// ITS#7713, change PAGEBASE to handle 65536 byte pages
#define PAGEBASE 0

// Number of nodes on a page
#define NUMKEYS(p) ((MP_LOWER(p) - (PAGEHDRSZ - PAGEBASE)) >> 1)

#define P_BRANCH 0x01    // branch page
#define P_LEAF 0x02      // leaf page
#define P_OVERFLOW 0x04  // overflow page
#define P_META 0x08      // meta page
#define P_DIRTY 0x10     // dirty page, also set for P_SUBP pages
#define P_LEAF2 0x20     // for MDB_DUPFIXED records
#define P_SUBP 0x40      // for MDB_DUPSORT sub-pages
#define P_LOOSE 0x4000   // page was dirtied then freed, can be reused
#define P_KEEP 0x8000    // leave this page alone during spill

// Page search flags
#define MDB_PS_MODIFY 1
#define MDB_PS_ROOTONLY 2
#define MDB_PS_FIRST 4
#define MDB_PS_LAST 8

#define MDB_SPLIT_REPLACE MDB_APPENDDUP  // newkey is not new

/* from mdb.c, for MDB_cursor */
#define C_INITIALIZED 0x01           // cursor has been initialized and is valid
#define C_EOF 0x02                   // No more data
#define C_SUB 0x04                   // Cursor is a sub-cursor
#define C_DEL 0x08                   // last op was a cursor_del
#define C_UNTRACK 0x40               // Un-track cursor when closing
#define C_WRITEMAP MDB_TXN_WRITEMAP  // Copy of txn flag
#define C_ORIG_RDONLY MDB_TXN_RDONLY

/* from mdb.c, for MDB_txn */
#define MDB_TXN_WRITEMAP MDB_WRITEMAP  // copy of MDB_env flag in writers
#define MDB_TXN_FINISHED 0x01          // txn is finished or never began
#define MDB_TXN_ERROR 0x02             // txn is unusable after an error
#define MDB_TXN_DIRTY 0x04             // must write, even if dirty list is empty
#define MDB_TXN_SPILLS 0x08            // txn or a parent has spilled pages
#define MDB_TXN_HAS_CHILD 0x10         // txn has an MDB_txn.mt_child
#define MDB_TXN_BLOCKED (MDB_TXN_FINISHED | MDB_TXN_ERROR | MDB_TXN_HAS_CHILD)

/* from mdb.c, for MDB_node */
#define F_BIGDATA 0x01  // data put on overflow page
#define F_SUBDATA 0x02  // data is a sub-database
#define F_DUPDATA 0x04  // data has duplicates
#define NODE_ADD_FLAGS (F_DUPDATA | F_SUBDATA | MDB_RESERVE | MDB_APPEND)

// The address of a key in a LEAF2 page.
// LEAF2 pages are used for MDB_DUPFIXED sorted-duplicate sub-DBs.
// There are no node headers, keys are stored contiguously.
#define LEAF2KEY(p, i, ks) (reinterpret_cast<char*>(p) + PAGEHDRSZ + ((i) * (ks)))

// The amount of space remaining in the page
#define SIZELEFT(p) (indx_t)(MP_UPPER(p) - MP_LOWER(p))

// The percentage of space used in the page, in tenths of a percent.
#define PAGEFILL(env, p) (1000L * ((env)->me_psize - PAGEHDRSZ - SIZELEFT(p)) / ((env)->me_psize - PAGEHDRSZ))
// The minimum page fill factor, in tenths of a percent.
// Pages emptier than this are candidates for merging.
#define FILL_THRESHOLD 250

// Test if a page is a leaf page
#define IS_LEAF(p) F_ISSET(MP_FLAGS(p), P_LEAF)
// Test if a page is a LEAF2 page
#define IS_LEAF2(p) F_ISSET(MP_FLAGS(p), P_LEAF2)
// Test if a page is a branch page
#define IS_BRANCH(p) F_ISSET(MP_FLAGS(p), P_BRANCH)
// Test if a page is an overflow page
#define IS_OVERFLOW(p) F_ISSET(MP_FLAGS(p), P_OVERFLOW)
// Test if a page is a sub page
#define IS_SUBP(p) F_ISSET(MP_FLAGS(p), P_SUBP)

// The number of overflow pages needed to store the given size.
#define OVPAGES(size, psize) ((PAGEHDRSZ - 1 + (size)) / (psize) + 1)

// Link in MDB_txn.mt_loose_pgs list.
// Kept outside the page header, which is needed when reusing the page.
#define NEXT_LOOSE_PAGE(p) (*(MDB_page**)((p) + 2))

// Header for a single key/data pair within a page.
// Used in pages of type P_BRANCH and P_LEAF without P_LEAF2.
// We guarantee 2-byte alignment for 'MDB_node's.
//
// mn_lo and mn_hi are used for data size on leaf nodes, and for child
// pgno on branch nodes. On 64 bit platforms, mn_flags is also used
// for pgno. (Branch nodes have no flags). Lo and hi are in host byte
// order in case some accesses can be optimized to 32-bit word access.
//
// Leaf node flags describe node contents. F_BIGDATA says the node's
// data part is the page number of an overflow page with actual data.
// F_DUPDATA and F_SUBDATA can be combined giving duplicate data in
// a sub-page/sub-database, and named databases (just F_SUBDATA).
struct MDB_node
{
    // part of data size or pgno
#if BYTE_ORDER == LITTLE_ENDIAN
    unsigned short mn_lo, mn_hi;
#else
    unsigned short mn_hi, mn_lo;
#endif
    unsigned short mn_flags;  // mdb_node
    unsigned short mn_ksize;  // key size
    char mn_data[1];          // key and data are appended here
};

// Size of the node header, excluding dynamic data at the end
#define NODESIZE offsetof(MDB_node, mn_data)

// Bit position of top word in page number, for shifting mn_flags
#define PGNO_TOPWORD ((pgno_t) - 1 > 0xffffffffu ? 32 : 0)

// Size of a node in a branch page with a given key.
// This is just the node header plus the key, there is no data.
#define INDXSIZE(k) (NODESIZE + ((k) == NULL ? 0 : (k)->mv_size))

// Size of a node in a leaf page with a given key and data.
// This is node header plus key plus data size.
#define LEAFSIZE(k, d) (NODESIZE + (k)->mv_size + (d)->mv_size)

// Address of node i in page p
#define NODEPTR(p, i) (reinterpret_cast<MDB_node*>(reinterpret_cast<char*>(p) + MP_PTRS(p)[i] + PAGEBASE))

// Address of the key for the node
#define NODEKEY(node) (reinterpret_cast<void*>((node)->mn_data))

// Address of the data for a node
#define NODEDATA(node) (reinterpret_cast<void*>(reinterpret_cast<char*>((node)->mn_data) + (node)->mn_ksize))

// Get the page number pointed to by a branch node
#define NODEPGNO(node)                                                                                                 \
    ((node)->mn_lo | ((pgno_t)(node)->mn_hi << 16) | (PGNO_TOPWORD ? ((pgno_t)(node)->mn_flags << PGNO_TOPWORD) : 0))
// Set the page number in a branch node
#define SETPGNO(node, pgno)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        (node)->mn_lo = (pgno) & 0xffff;                                                                               \
        (node)->mn_hi = (pgno) >> 16;                                                                                  \
        if (PGNO_TOPWORD)                                                                                              \
            (node)->mn_flags = (pgno) >> PGNO_TOPWORD;                                                                 \
    } while (0)

// Get the size of the data in a leaf node
#define NODEDSZ(node) ((node)->mn_lo | ((unsigned)(node)->mn_hi << 16))
// Set the size of the data for a leaf node
#define SETDSZ(node, size)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        (node)->mn_lo = (size) & 0xffff;                                                                               \
        (node)->mn_hi = (size) >> 16;                                                                                  \
    } while (0)
// The size of a key in a node
#define NODEKSZ(node) ((node)->mn_ksize)

// Copy a page number from src to dst
#ifdef MISALIGNED_OK
#define COPY_PGNO(dst, src) dst = src
#undef MP_PGNO
#define MP_PGNO(p) ((p)->mp_pgno)
#else
#if MDB_SIZE_MAX > 0xffffffffU
#define COPY_PGNO(dst, src)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        unsigned short *s, *d;                                                                                         \
        s = (unsigned short*)&(src);                                                                                   \
        d = (unsigned short*)&(dst);                                                                                   \
        *d++ = *s++;                                                                                                   \
        *d++ = *s++;                                                                                                   \
        *d++ = *s++;                                                                                                   \
        *d = *s;                                                                                                       \
    } while (0)
#else
#define COPY_PGNO(dst, src)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        unsigned short *s, *d;                                                                                         \
        s = (unsigned short*)&(src);                                                                                   \
        d = (unsigned short*)&(dst);                                                                                   \
        *d++ = *s++;                                                                                                   \
        *d = *s;                                                                                                       \
    } while (0)
#endif
#endif

// Set the node's key into keyptr, if requested.
#define MDB_GET_KEY(node, keyptr)                                                                                      \
    {                                                                                                                  \
        if ((keyptr) != NULL)                                                                                          \
        {                                                                                                              \
            (keyptr)->mv_size = NODEKSZ(node);                                                                         \
            (keyptr)->mv_data = NODEKEY(node);                                                                         \
        }                                                                                                              \
    }

// Set the node's key into key.
#define MDB_GET_KEY2(node, key)                                                                                        \
    {                                                                                                                  \
        (key).mv_size = NODEKSZ(node);                                                                                 \
        (key).mv_data = NODEKEY(node);                                                                                 \
    }

// Page Management Functions
int mdb_page_alloc(MDB_cursor* mc, int num, MDB_page** mp);
int mdb_page_new(MDB_cursor* mc, uint32_t flags, int num, MDB_page** mp);
int mdb_page_touch(MDB_cursor* mc);
int mdb_page_unspill(MDB_txn* txn, MDB_page* mp, MDB_page** ret);
int mdb_page_get(MDB_cursor* mc, pgno_t pgno, MDB_page** mp, int* lvl);
int mdb_page_search_root(MDB_cursor* mc, MDB_val* key, int modify);
int mdb_page_search(MDB_cursor* mc, MDB_val* key, int flags);
int mdb_page_merge(MDB_cursor* csrc, MDB_cursor* cdst);
int mdb_page_split(MDB_cursor* mc, MDB_val* newkey, MDB_val* newdata, pgno_t newpgno, unsigned int nflags);
void mdb_page_copy(MDB_page* dst, MDB_page* src, unsigned int psize);
int mdb_page_flush(MDB_txn* txn, int keep);
// Node operation functions
MDB_node* mdb_node_search(MDB_cursor* mc, MDB_val* key, int* exactp);
int mdb_node_add(MDB_cursor* mc, indx_t indx, MDB_val* key, MDB_val* data, pgno_t pgno, unsigned int flags);
void mdb_node_del(MDB_cursor* mc, int ksize);
void mdb_node_shrink(MDB_page* mp, indx_t indx);
int mdb_node_move(MDB_cursor* csrc, MDB_cursor* cdst, int fromleft);
int mdb_node_read(MDB_cursor* mc, MDB_node* leaf, MDB_val* data);
size_t mdb_leaf_size(MDB_env* env, MDB_val* key, MDB_val* data);
size_t mdb_branch_size(MDB_env* env, MDB_val* key);
int mdb_ovpage_free(MDB_cursor* mc, MDB_page* mp);
int mdb_page_spill(MDB_cursor* m0, MDB_val* key, MDB_val* data);
void mdb_page_dirty(MDB_txn* txn, MDB_page* mp);
MDB_page* mdb_page_malloc(MDB_txn* txn, unsigned num);
void mdb_dpage_free(MDB_env* env, MDB_page* dp);
