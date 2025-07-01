#include "debug.h"

#include "btree.h"
#include "env.h"
#include "page.h"
#include "txn.h"

#if FDS_DEBUG
int fds_debug;
txnid_t fds_debug_start;
#endif

#ifndef NDEBUG
void ESECT fds_assert_fail(FDS_env* env, const char* expr_txt, const char* func, const char* file, int line)
{
    char buf[400]{};

    // C99-style, size-bounded formatting
    int n{snprintf(buf, sizeof(buf), "%.100s:%d: Assertion '%.200s' failed in %.40s()", file, line, expr_txt, func)};

    // guarantee a terminator even on pathological libraries
    if (n < 0 || static_cast<size_t>(n) >= sizeof(buf))
        buf[sizeof(buf) - 1] = '\0';

    if (env->me_assert_func != nullptr)
        env->me_assert_func(env, buf);

    fprintf(stderr, "%s\n", buf);
    abort();
}
#endif /* NDEBUG */

#if FDS_DEBUG
// Return the page number of mp which may be sub-page, for debug output
auto fds_dbg_pgno(FDS_page* mp) -> pgno_t
{
    pgno_t ret{};
    COPY_PGNO(ret, MP_PGNO(mp));
    return ret;
}

// Display a key in hexadecimal and return the address of the result.
// key the key to display
// buf the buffer to write into. Should always be DKBUF.
// The key in hexadecimal form.
auto fds_dkey(FDS_val* key, char* buf) -> char*
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    if (key == nullptr)
        return (char*)"";

    if (key->mv_size > DKBUF_MAXKEYSIZE)
        return (char*)"FDS_MAXKEYSIZE";

    // may want to make this a dynamic check: if the key is mostly
    // printable characters, print it as-is instead of converting to hex.
    char* ptr{buf};
    unsigned char* c{(unsigned char*)key->mv_data};
    buf[0] = '\0';
    for (unsigned int i{}; i < key->mv_size; ++i)
        ptr += sprintf(ptr, "%02x", *c++);
    return buf;

#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

auto fds_dval(FDS_txn* txn, FDS_dbi dbi, FDS_val* data, char* buf) -> char*
{
    *buf = '\0';
    return buf;
}

auto fds_leafnode_type(FDS_node* n) -> const char*
{
    static const char* const tp[2][2] = {
        {          "",     ": DB"},
        {": sub-page", ": sub-DB"}
    };
    return F_ISSET(n->mn_flags, F_BIGDATA) ? ": overflow page"
                                           : tp[F_ISSET(n->mn_flags, F_DUPDATA)][F_ISSET(n->mn_flags, F_SUBDATA)];
}

// Display all the keys in the page.
void fds_page_list(FDS_page* mp)
{
    pgno_t pgno{fds_dbg_pgno(mp)};
    const char* state{((MP_FLAGS(mp) & P_DIRTY) != 0) ? ", dirty" : ""};
    const char* type{nullptr};
    DKBUF;

    switch (MP_FLAGS(mp) & (P_BRANCH | P_LEAF | P_META | P_OVERFLOW | P_SUBP))
    {
    case P_BRANCH:
        type = "Branch page";
        break;
    case P_LEAF:
        type = "Leaf page";
        break;
    case P_OVERFLOW:
        fprintf(stderr, "Overflow page %" Yu " pages %u%s\n", pgno, mp->mp_pages, state);
        return;
    case P_META:
        fprintf(stderr,
                "Meta-page %" Yu " txnid %" Yu "\n",
                pgno,
                reinterpret_cast<FDS_meta*>(reinterpret_cast<char*>(mp) + PAGEHDRSZ)->mm_txnid);
        return;
    default:
        fprintf(stderr, "Bad page %" Yu " flags 0x%X\n", pgno, MP_FLAGS(mp));
        return;
    }

    unsigned int nkeys{NUMKEYS(mp)};
    fprintf(stderr, "%s %" Yu " numkeys %d%s\n", type, pgno, nkeys, state);

    unsigned int total{0};
    for (unsigned int i{0}; i < nkeys; i++)
    {
        FDS_node* node{NODEPTR(mp, i)};
        FDS_val key{};
        key.mv_size = node->mn_ksize;
        key.mv_data = node->mn_data;
        unsigned int nsize{unsigned(NODESIZE + key.mv_size)};
        if (IS_BRANCH(mp))
        {
            fprintf(stderr, "key %d: page %" Yu ", %s\n", i, NODEPGNO(node), DKEY(&key));
            total += nsize;
        }
        else
        {
            if (F_ISSET(node->mn_flags, F_BIGDATA))
                nsize += sizeof(pgno_t);
            else
                nsize += NODEDSZ(node);
            total += nsize;
            nsize += sizeof(indx_t);
            fprintf(stderr, "key %d: nsize %d, %s%s\n", i, nsize, DKEY(&key), fds_leafnode_type(node));
        }
        total = EVEN(total);
    }
    fprintf(stderr, "Total: header %d + contents %d + unused %d\n", PAGEBASE + MP_LOWER(mp), total, SIZELEFT(mp));
}
#endif

// Count all the pages in each DB and in the freelist
// and make sure it matches the actual number of pages
// being used.
// All named DBs must be open for a correct count.
void fds_audit(FDS_txn* txn)
{
    FDS_cursor mc{};
    FDS_val key{}, data{};

    FDS_ID freecount{0};
    fds_cursor_init(&mc, txn, FREE_DBI);
    int rc{};
    while ((rc = fds_cursor_get(&mc, &key, &data, FDS_NEXT)) == 0)
        freecount += *(FDS_ID*)data.mv_data;
    fds_tassert(txn, rc == FDS_NOTFOUND);

    FDS_ID count{0};
    for (FDS_dbi i{0}; i < txn->mt_numdbs; i++)
    {
        if (!(txn->mt_dbflags[i] & DB_VALID))
            continue;
        fds_cursor_init(&mc, txn, i);
        if (txn->mt_dbs[i].md_root == P_INVALID)
            continue;
        count += txn->mt_dbs[i].md_branch_pages + txn->mt_dbs[i].md_leaf_pages + txn->mt_dbs[i].md_overflow_pages;
    }
    if (freecount + count + NUM_METAS != txn->mt_next_pgno)
    {
        fprintf(stderr,
                "audit: %" Yu " freecount: %" Yu " count: %" Yu " total: %" Yu " next_pgno: %" Yu "\n",
                txn->mt_txnid,
                freecount,
                count + NUM_METAS,
                freecount + count + NUM_METAS,
                txn->mt_next_pgno);
    }
}
