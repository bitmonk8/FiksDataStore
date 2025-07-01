#include "compare.h"

#include "db.h"
#include "txn.h"

// Compare two items pointing at aligned fds_size_t's
auto fds_cmp_long(const FDS_val* a, const FDS_val* b) -> int
{
    return (*(fds_size_t*)a->mv_data < *(fds_size_t*)b->mv_data)
               ? -1
               : static_cast<int>(*(fds_size_t*)a->mv_data > *(fds_size_t*)b->mv_data);
}

// Compare two items lexically
auto fds_cmp_memn(const FDS_val* a, const FDS_val* b) -> int
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
auto fds_cmp_memnr(const FDS_val* a, const FDS_val* b) -> int
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

auto fds_cmp(FDS_txn* txn, FDS_dbi dbi, const FDS_val* a, const FDS_val* b) -> int
{
    return txn->mt_dbxs[dbi].md_cmp(a, b);
}
