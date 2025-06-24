#include "mdb_compare.h"

#include "mdb_db.h"
#include "mdb_txn.h"

// Compare two items pointing at aligned mdb_size_t's
int mdb_cmp_long(const MDB_val* a, const MDB_val* b)
{
    return (*(mdb_size_t*)a->mv_data < *(mdb_size_t*)b->mv_data)
               ? -1
               : static_cast<int>(*(mdb_size_t*)a->mv_data > *(mdb_size_t*)b->mv_data);
}

// Compare two items pointing at aligned unsigned int's.
int mdb_cmp_int(const MDB_val* a, const MDB_val* b)
{
    return (*(unsigned int*)a->mv_data < *(unsigned int*)b->mv_data)
               ? -1
               : static_cast<int>(*(unsigned int*)a->mv_data > *(unsigned int*)b->mv_data);
}

// Compare two items pointing at unsigned ints of unknown alignment.
// Nodes and keys are guaranteed to be 2-byte aligned.
int mdb_cmp_cint(const MDB_val* a, const MDB_val* b)
{
#if BYTE_ORDER == LITTLE_ENDIAN
    unsigned short* u = (unsigned short*)((char*)a->mv_data + a->mv_size);
    unsigned short* c = (unsigned short*)((char*)b->mv_data + a->mv_size);
    do
    {
        int x{*--u - *--c};
        if (x != 0)
            return x;
    } while (u > (unsigned short*)a->mv_data);
    return 0;
#else
    unsigned short* end = (unsigned short*)((char*)a->mv_data + a->mv_size);
    unsigned short* u = (unsigned short*)a->mv_data;
    unsigned short* c = (unsigned short*)b->mv_data;
    do
    {
        int x{*u++ - *c++};
        if (x)
            return x;
    } while (u < end);
    return 0;
#endif
}

// Compare two items lexically
int mdb_cmp_memn(const MDB_val* a, const MDB_val* b)
{
    unsigned int len = a->mv_size;
    ssize_t len_diff{(ssize_t)a->mv_size - (ssize_t)b->mv_size};
    if (len_diff > 0)
    {
        len = b->mv_size;
        len_diff = 1;
    }

    int diff{memcmp(a->mv_data, b->mv_data, len)};
    return (diff != 0) ? diff : len_diff < 0 ? -1 : len_diff;
}

// Compare two items in reverse byte order
int mdb_cmp_memnr(const MDB_val* a, const MDB_val* b)
{
    const unsigned char* p1_lim{(const unsigned char*)a->mv_data};
    const unsigned char* p1{(const unsigned char*)a->mv_data + a->mv_size};
    const unsigned char* p2{(const unsigned char*)b->mv_data + b->mv_size};

    ssize_t len_diff{(ssize_t)a->mv_size - (ssize_t)b->mv_size};
    if (len_diff > 0)
    {
        p1_lim += len_diff;
        len_diff = 1;
    }

    while (p1 > p1_lim)
    {
        int diff{*--p1 - *--p2};
        if (diff != 0)
            return diff;
    }
    return len_diff < 0 ? -1 : len_diff;
}

int mdb_cmp(MDB_txn* txn, MDB_dbi dbi, const MDB_val* a, const MDB_val* b)
{
    return txn->mt_dbxs[dbi].md_cmp(a, b);
}

int mdb_dcmp(MDB_txn* txn, MDB_dbi dbi, const MDB_val* a, const MDB_val* b)
{
    MDB_cmp_func* dcmp = txn->mt_dbxs[dbi].md_dcmp;
    if (NEED_CMP_CLONG(dcmp, a->mv_size))
        dcmp = mdb_cmp_clong;
    return dcmp(a, b);
}
