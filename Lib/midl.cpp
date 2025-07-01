// midl.c
// ldap bdb back-end ID List functions
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

#include "midl.h"

#include <sys/types.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

// LMDB Internals
//
// ID List Management
//
#define CMP(x, y) ((x) < (y) ? -1 : (x) > (y))

auto fds_midl_search(const FDS_IDL ids, FDS_ID id) -> unsigned
{
    //
    // binary search of id in ids
    // if found, returns position of id
    // if not found, returns first position greater than id
    //
    unsigned base = 0;
    unsigned cursor = 1;
    int val = 0;
    auto n = (unsigned)ids[0];

    while (0 < n)
    {
        unsigned pivot{n >> 1};
        cursor = base + pivot + 1;
        val = CMP(ids[cursor], id);

        if (val < 0)
        {
            n = pivot;
        }
        else if (val > 0)
        {
            base = cursor;
            n -= pivot + 1;
        }
        else
        {
            return cursor;
        }
    }

    if (val > 0)
    {
        ++cursor;
    }
    return cursor;
}

auto fds_midl_alloc(int num) -> FDS_IDL
{
    auto ids = (FDS_IDL)malloc((num + 2) * sizeof(FDS_ID));
    if (ids != nullptr)
    {
        *ids++ = num;
        *ids = 0;
    }
    return ids;
}

void fds_midl_free(FDS_IDL ids)
{
    if (ids != nullptr)
        free(ids - 1);
}

void fds_midl_shrink(FDS_IDL* idp)
{
    FDS_IDL ids = *idp;
    --ids;
    if (*ids > FDS_IDL_UM_MAX)
    {
        auto new_ids = (FDS_IDL)realloc(ids, (FDS_IDL_UM_MAX + 2) * sizeof(FDS_ID));
        if (new_ids != nullptr)
        {
            ids = new_ids;
            *ids++ = FDS_IDL_UM_MAX;
            *idp = ids;
        }
    }
}

static auto fds_midl_grow(FDS_IDL* idp, int num) -> int
{
    FDS_IDL idn = *idp - 1;
    /* grow it */
    auto new_idn = (FDS_IDL)realloc(idn, (*idn + num + 2) * sizeof(FDS_ID));
    if (new_idn == nullptr)
        return ENOMEM;
    idn = new_idn;
    *idn++ += num;
    *idp = idn;
    return 0;
}

auto fds_midl_need(FDS_IDL* idp, unsigned num) -> int
{
    FDS_IDL ids = *idp;
    num += (unsigned)ids[0];
    if (num > ids[-1])
    {
        num = (num + num / 4 + (256 + 2)) & -256;
        ids = (FDS_IDL)realloc(ids - 1, num * sizeof(FDS_ID));
        if (ids == nullptr)
            return ENOMEM;
        *ids++ = num - 2;
        *idp = ids;
    }
    return 0;
}

auto fds_midl_append(FDS_IDL* idp, FDS_ID id) -> int
{
    FDS_IDL ids = *idp;
    /* Too big? */
    if (ids[0] >= ids[-1])
    {
        if (fds_midl_grow(idp, FDS_IDL_UM_MAX) != 0)
            return ENOMEM;
        ids = *idp;
    }
    ids[0]++;
    ids[ids[0]] = id;
    return 0;
}

auto fds_midl_append_list(FDS_IDL* idp, FDS_IDL app) -> int
{
    FDS_IDL ids = *idp;
    /* Too big? */
    if (ids[0] + app[0] >= ids[-1])
    {
        if (fds_midl_grow(idp, (int)app[0]) != 0)
            return ENOMEM;
        ids = *idp;
    }
    memcpy(&ids[ids[0] + 1], &app[1], app[0] * sizeof(FDS_ID));
    ids[0] += app[0];
    return 0;
}

auto fds_midl_append_range(FDS_IDL* idp, FDS_ID id, unsigned n) -> int
{
    FDS_ID* ids = *idp;
    FDS_ID len = ids[0];
    /* Too big? */
    if (len + n > ids[-1])
    {
        if (fds_midl_grow(idp, n | FDS_IDL_UM_MAX) != 0)
            return ENOMEM;
        ids = *idp;
    }
    ids[0] = len + n;
    ids += len;
    while (n != 0U)
        ids[n--] = id++;
    return 0;
}

void fds_midl_xmerge(FDS_IDL idl, const FDS_IDL merge)
{
    const FDS_ID merge_count = merge[0];
    const FDS_ID idl_count = idl[0];
    const FDS_ID total_count = merge_count + idl_count;

    idl[0] = (FDS_ID)-1; /* delimiter for idl scan below */

    FDS_ID remaining_merge = merge_count;
    FDS_ID remaining_idl = idl_count;
    FDS_ID write_pos = total_count;
    FDS_ID current_idl_value = idl[remaining_idl];

    while (remaining_merge != 0U)
    {
        const FDS_ID current_merge_value = merge[remaining_merge--];
        for (; current_idl_value < current_merge_value; current_idl_value = idl[--remaining_idl])
            idl[write_pos--] = current_idl_value;
        idl[write_pos--] = current_merge_value;
    }
    idl[0] = total_count;
}

/* Quicksort + Insertion sort for small arrays */

enum
{
    SMALL = 8
};
#define MIDL_SWAP(a, b)                                                                                                \
    {                                                                                                                  \
        itmp = (a);                                                                                                    \
        (a) = (b);                                                                                                     \
        (b) = itmp;                                                                                                    \
    }

void fds_midl_sort(FDS_IDL ids)
{
    /* Max possible depth of int-indexed tree * 2 items/level */
    int istack[sizeof(int) * CHAR_BIT * 2];
    int i{};
    int j{};
    int k{};
    int l{1};
    int ir{(int)ids[0]};
    int jstack{0};

    for (;;)
    {
        if (ir - l < SMALL)
        { /* Insertion sort */
            for (j = l + 1; j <= ir; j++)
            {
                FDS_ID a{ids[j]};
                for (i = j - 1; i >= 1; i--)
                {
                    if (ids[i] >= a)
                        break;
                    ids[i + 1] = ids[i];
                }
                ids[i + 1] = a;
            }
            if (jstack == 0)
                break;
            ir = istack[jstack--];
            l = istack[jstack--];
        }
        else
        {
            int k{(l + ir) >> 1}; /* Choose median of left, center, right */
            FDS_ID itmp{};
            MIDL_SWAP(ids[k], ids[l + 1]);
            if (ids[l] < ids[ir])
            {
                MIDL_SWAP(ids[l], ids[ir]);
            }
            if (ids[l + 1] < ids[ir])
            {
                MIDL_SWAP(ids[l + 1], ids[ir]);
            }
            if (ids[l] < ids[l + 1])
            {
                MIDL_SWAP(ids[l], ids[l + 1]);
            }
            i = l + 1;
            j = ir;
            FDS_ID a{ids[l + 1]};
            for (;;)
            {
                do
                    i++;
                while (ids[i] > a);
                do
                    j--;
                while (ids[j] < a);
                if (j < i)
                    break;
                MIDL_SWAP(ids[i], ids[j]);
            }
            ids[l + 1] = ids[j];
            ids[j] = a;
            jstack += 2;
            if (ir - i + 1 >= j - l)
            {
                istack[jstack] = ir;
                istack[jstack - 1] = i;
                ir = j - 1;
            }
            else
            {
                istack[jstack] = j - 1;
                istack[jstack - 1] = l;
                l = i;
            }
        }
    }
}

auto fds_mid2l_search(FDS_ID2L ids, FDS_ID id) -> unsigned
{
    // binary search of id in ids
    // if found, returns position of id
    // if not found, returns first position greater than id
    unsigned base = 0;
    unsigned cursor = 1;
    int val = 0;
    auto n = (unsigned)ids[0].mid;

    while (0 < n)
    {
        unsigned pivot{n >> 1};
        cursor = base + pivot + 1;
        val = CMP(id, ids[cursor].mid);

        if (val < 0)
        {
            n = pivot;
        }
        else if (val > 0)
        {
            base = cursor;
            n -= pivot + 1;
        }
        else
        {
            return cursor;
        }
    }

    if (val > 0)
    {
        ++cursor;
    }
    return cursor;
}

auto fds_mid2l_insert(FDS_ID2L ids, FDS_ID2* id) -> int
{
    unsigned x{fds_mid2l_search(ids, id->mid)};

    if (x < 1)
    {
        /* internal error */
        return -2;
    }

    if (x <= ids[0].mid && ids[x].mid == id->mid)
    {
        /* duplicate */
        return -1;
    }

    if (ids[0].mid >= FDS_IDL_UM_MAX)
    {
        /* too big */
        return -2;
    }

    /* insert id */
    ids[0].mid++;
    for (unsigned i{(unsigned)ids[0].mid}; i > x; i--)
        ids[i] = ids[i - 1];
    ids[x] = *id;

    return 0;
}

auto fds_mid2l_append(FDS_ID2L ids, FDS_ID2* id) -> int
{
    /* Too big? */
    if (ids[0].mid >= FDS_IDL_UM_MAX)
    {
        return -2;
    }
    ids[0].mid++;
    ids[ids[0].mid] = *id;
    return 0;
}
