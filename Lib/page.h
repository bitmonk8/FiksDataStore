#pragma once

#include "internal.h"
#include "midl.h"
#include "util.h"

// Common header for all page types. The page type depends on mp_flags.
struct FDS_page
{
#define mp_pgno mp_p.p_pgno
#define mp_next mp_p.p_next
    union
    {
        pgno_t p_pgno;            // page number
        struct FDS_page* p_next;  // for in-memory list of freed pages
    } mp_p;
    uint16_t mp_pad;    // padding for alignment
    uint16_t mp_flags;  // fds_page
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
struct FDS_page2
{
    uint16_t mp2_p[sizeof(pgno_t) / 2];
    uint16_t mp2_pad;
    uint16_t mp2_flags;
    indx_t mp2_lower;
    indx_t mp2_upper;
    indx_t mp2_ptrs[0];
};

#define MP_PGNO(p) (reinterpret_cast<FDS_page2*>(p)->mp2_p)
#define MP_PAD(p) (reinterpret_cast<FDS_page2*>(p)->mp2_pad)
#define MP_FLAGS(p) (reinterpret_cast<const FDS_page2*>(p)->mp2_flags)
#define MP_LOWER(p) (reinterpret_cast<FDS_page2*>(p)->mp2_lower)
#define MP_UPPER(p) (reinterpret_cast<FDS_page2*>(p)->mp2_upper)
#define MP_PTRS(p) (reinterpret_cast<FDS_page2*>(p)->mp2_ptrs)

// Size of the page header, excluding dynamic data at the end
#define PAGEHDRSZ ((unsigned)offsetof(FDS_page, mp_ptrs))

// Address of first usable data byte in a page, after the header
#define METADATA(p) (reinterpret_cast<void*>(reinterpret_cast<char*>(p) + PAGEHDRSZ))

#define PAGEBASE 0

// Number of nodes on a page
#define NUMKEYS(p) ((MP_LOWER(p) - (PAGEHDRSZ - PAGEBASE)) >> 1)

#define P_BRANCH 0x01    // branch page
#define P_LEAF 0x02      // leaf page
#define P_OVERFLOW 0x04  // overflow page
#define P_META 0x08      // meta page
#define P_DIRTY 0x10     // dirty page
#define P_SUBP 0x02
#define P_LOOSE 0x4000  // page was dirtied then freed, can be reused
#define P_KEEP 0x8000   // leave this page alone during spill

constexpr int F_BIGDATA = 0x01;  // data put on overflow page
constexpr int F_SUBDATA = 0x02;  // data is a sub-database
constexpr int F_DUPDATA = 0x04;

// The amount of space remaining in the page
#define SIZELEFT(p) (indx_t)(MP_UPPER(p) - MP_LOWER(p))

// The percentage of space used in the page, in tenths of a percent.
#define PAGEFILL(env, p) (1000.0L * ((env)->me_psize - PAGEHDRSZ - SIZELEFT(p)) / ((env)->me_psize - PAGEHDRSZ))
// The minimum page fill factor, in tenths of a percent.
// Pages emptier than this are candidates for merging.
constexpr int FILL_THRESHOLD = 250;

// Test if a page is a leaf page
#define IS_LEAF(p) F_ISSET(MP_FLAGS(p), P_LEAF)
// Test if a page is a branch page
#define IS_BRANCH(p) F_ISSET(MP_FLAGS(p), P_BRANCH)
// Test if a page is an overflow page
#define IS_OVERFLOW(p) F_ISSET(MP_FLAGS(p), P_OVERFLOW)

// The number of overflow pages needed to store the given size.
#define OVPAGES(size, psize) (((PAGEHDRSZ - 1 + (size)) / (psize)) + 1)

// Link in FDS_txn.mt_loose_pgs list.
// Kept outside the page header, which is needed when reusing the page.
#define NEXT_LOOSE_PAGE(p) (*(FDS_page**)((p) + 2))

// Header for a single key/data pair within a page.
// Used in pages of type P_BRANCH and P_LEAF.
// We guarantee 2-byte alignment for 'FDS_node's.
//
// mn_lo and mn_hi are used for data size on leaf nodes, and for child
// pgno on branch nodes. On 64 bit platforms, mn_flags is also used
// for pgno. (Branch nodes have no flags). Lo and hi are in host byte
// order in case some accesses can be optimized to 32-bit word access.
//
// Leaf node flags describe node contents. F_BIGDATA says the node's
// data part is the page number of an overflow page with actual data.
// F_SUBDATA indicates named databases.
struct FDS_node
{
    // part of data size or pgno
#if BYTE_ORDER == LITTLE_ENDIAN
    unsigned short mn_lo, mn_hi;
#else
    unsigned short mn_hi, mn_lo;
#endif
    unsigned short mn_flags;  // fds_node
    unsigned short mn_ksize;  // key size
    char mn_data[1];          // key and data are appended here
};

// Size of the node header, excluding dynamic data at the end
#define NODESIZE offsetof(FDS_node, mn_data)

// Bit position of top word in page number, for shifting mn_flags
#define PGNO_TOPWORD ((pgno_t) - 1 > 0xffffffffU ? 32 : 0)

// Size of a node in a branch page with a given key.
// This is just the node header plus the key, there is no data.
#define INDXSIZE(k) (NODESIZE + ((k) == NULL ? 0 : (k)->mv_size))

// Size of a node in a leaf page with a given key and data.
// This is node header plus key plus data size.
#define LEAFSIZE(k, d) (NODESIZE + (k)->mv_size + (d)->mv_size)

// Address of node i in page p
#define NODEPTR(p, i) (reinterpret_cast<FDS_node*>(reinterpret_cast<char*>(p) + MP_PTRS(p)[i] + PAGEBASE))

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
#if SIZE_MAX > 0xffffffffU
#define COPY_PGNO(dst, src)                                                                                            \
    do                                                                                                                 \
    {                                                                                                                  \
        unsigned short* s;                                                                                             \
        unsigned short* d;                                                                                             \
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
        unsigned short* s;                                                                                             \
        unsigned short* d;                                                                                             \
        s = (unsigned short*)&(src);                                                                                   \
        d = (unsigned short*)&(dst);                                                                                   \
        *d++ = *s++;                                                                                                   \
        *d = *s;                                                                                                       \
    } while (0)
#endif
#endif

// Set the node's key into keyptr, if requested.
#define FDS_GET_KEY(node, keyptr)                                                                                      \
    {                                                                                                                  \
        if ((keyptr) != NULL)                                                                                          \
        {                                                                                                              \
            (keyptr)->mv_size = NODEKSZ(node);                                                                         \
            (keyptr)->mv_data = NODEKEY(node);                                                                         \
        }                                                                                                              \
    }

// Set the node's key into key.
#define FDS_GET_KEY2(node, key)                                                                                        \
    {                                                                                                                  \
        (key).mv_size = NODEKSZ(node);                                                                                 \
        (key).mv_data = NODEKEY(node);                                                                                 \
    }
