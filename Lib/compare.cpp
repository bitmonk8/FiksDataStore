#include "compare.h"

#include "db.h"
#include "txn.h"

// Compare two items pointing at aligned size_t's
auto fds_cmp_long(const FDS_val* a, const FDS_val* b) -> int
{
    return (*(size_t*)a->mv_data < *(size_t*)b->mv_data)
               ? -1
               : static_cast<int>(*(size_t*)a->mv_data > *(size_t*)b->mv_data);
}

// Compare two items lexically
auto fds_cmp_memn(const FDS_val* a, const FDS_val* b) -> int
{
    const size_t size_a = a->mv_size;
    const size_t size_b = b->mv_size;
    const size_t min_len = (size_a < size_b) ? size_a : size_b;
    const int diff = memcmp(a->mv_data, b->mv_data, min_len);

    if (diff != 0)
    {
        return diff;
    }
    if (size_a == size_b)
    {
        return 0;
    }
    return size_a > size_b ? 1 : -1;
}

// Compare two items in reverse byte order
auto fds_cmp_memnr(const FDS_val* a, const FDS_val* b) -> int
{
    const auto size_a = a->mv_size;
    const auto size_b = b->mv_size;
    const auto min_size = (size_a < size_b) ? size_a : size_b;
    const unsigned char* p1 = static_cast<const unsigned char*>(a->mv_data) + size_a;
    const unsigned char* p2 = static_cast<const unsigned char*>(b->mv_data) + size_b;

    for (size_t i = 0; i < min_size; ++i)
    {
        const int diff = *--p1 - *--p2;
        if (diff != 0)
        {
            return diff;
        }
    }

    if (size_a == size_b)
    {
        return 0;
    }
    return size_a > size_b ? 1 : -1;
}

auto fds_cmp(FDS_txn* txn, FDS_dbi dbi, const FDS_val* a, const FDS_val* b) -> int
{
    return txn->mt_dbxs[dbi].md_cmp(a, b);
}
