#include "mdb_debug.h"

#include "mdb_env.h"

#if MDB_DEBUG
int mdb_debug = MDB_DBG_TRACE;
txnid_t mdb_debug_start;
#endif

#ifndef NDEBUG
void ESECT mdb_assert_fail(MDB_env* env, const char* expr_txt, const char* func, const char* file, int line)
{
    char buf[400];

    // C99-style, size-bounded formatting
    int n = snprintf(buf, sizeof(buf), "%.100s:%d: Assertion '%.200s' failed in %.40s()", file, line, expr_txt, func);

    // guarantee a terminator even on pathological libraries
    if (n < 0 || (size_t)n >= sizeof(buf))
        buf[sizeof(buf) - 1] = '\0';

    if (env->me_assert_func)
        env->me_assert_func(env, buf);

    fprintf(stderr, "%s\n", buf);
    abort();
}
#endif /* NDEBUG */

#if MDB_DEBUG
// Return the page number of mp which may be sub-page, for debug output
pgno_t mdb_dbg_pgno(MDB_page* mp)
{
    pgno_t ret;
    COPY_PGNO(ret, MP_PGNO(mp));
    return ret;
}

// Display a key in hexadecimal and return the address of the result.
// key the key to display
// buf the buffer to write into. Should always be DKBUF.
// The key in hexadecimal form.
char* mdb_dkey(MDB_val* key, char* buf)
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif
    char* ptr = buf;
    unsigned char* c = key->mv_data;
    unsigned int i;

    if (!key)
        return "";

    if (key->mv_size > DKBUF_MAXKEYSIZE)
        return "MDB_MAXKEYSIZE";
    // may want to make this a dynamic check: if the key is mostly
    // printable characters, print it as-is instead of converting to hex.
    buf[0] = '\0';
    for (i = 0; i < key->mv_size; i++)
        ptr += sprintf(ptr, "%02x", *c++);
    return buf;

#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

char* mdb_dval(MDB_txn* txn, MDB_dbi dbi, MDB_val* data, char* buf)
{
    if (txn->mt_dbs[dbi].md_flags & MDB_DUPSORT)
    {
        mdb_dkey(data, buf + 1);
        *buf = '[';
        strcpy(buf + data->mv_size * 2 + 1, "]");
    }
    else
        *buf = '\0';
    return buf;
}

const char* mdb_leafnode_type(MDB_node* n)
{
    static char* const tp[2][2] = {
        {          "",     ": DB"},
        {": sub-page", ": sub-DB"}
    };
    return F_ISSET(n->mn_flags, F_BIGDATA) ? ": overflow page"
                                           : tp[F_ISSET(n->mn_flags, F_DUPDATA)][F_ISSET(n->mn_flags, F_SUBDATA)];
}

// Display all the keys in the page.
void mdb_page_list(MDB_page* mp)
{
    pgno_t pgno = mdb_dbg_pgno(mp);
    const char *type, *state = (MP_FLAGS(mp) & P_DIRTY) ? ", dirty" : "";
    MDB_node* node;
    unsigned int i, nkeys, nsize, total = 0;
    MDB_val key;
    DKBUF;

    switch (MP_FLAGS(mp) & (P_BRANCH | P_LEAF | P_LEAF2 | P_META | P_OVERFLOW | P_SUBP))
    {
    case P_BRANCH:
        type = "Branch page";
        break;
    case P_LEAF:
        type = "Leaf page";
        break;
    case P_LEAF | P_SUBP:
        type = "Sub-page";
        break;
    case P_LEAF | P_LEAF2:
        type = "LEAF2 page";
        break;
    case P_LEAF | P_LEAF2 | P_SUBP:
        type = "LEAF2 sub-page";
        break;
    case P_OVERFLOW:
        fprintf(stderr, "Overflow page %" Yu " pages %u%s\n", pgno, mp->mp_pages, state);
        return;
    case P_META:
        fprintf(stderr, "Meta-page %" Yu " txnid %" Yu "\n", pgno, ((MDB_meta*)METADATA(mp))->mm_txnid);
        return;
    default:
        fprintf(stderr, "Bad page %" Yu " flags 0x%X\n", pgno, MP_FLAGS(mp));
        return;
    }

    nkeys = NUMKEYS(mp);
    fprintf(stderr, "%s %" Yu " numkeys %d%s\n", type, pgno, nkeys, state);

    for (i = 0; i < nkeys; i++)
    {
        if (IS_LEAF2(mp))
        {  // LEAF2 pages have no mp_ptrs[] or node headers
            key.mv_size = nsize = mp->mp_pad;
            key.mv_data = LEAF2KEY(mp, i, nsize);
            total += nsize;
            fprintf(stderr, "key %d: nsize %d, %s\n", i, nsize, DKEY(&key));
            continue;
        }
        node = NODEPTR(mp, i);
        key.mv_size = node->mn_ksize;
        key.mv_data = node->mn_data;
        nsize = NODESIZE + key.mv_size;
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
            fprintf(stderr, "key %d: nsize %d, %s%s\n", i, nsize, DKEY(&key), mdb_leafnode_type(node));
        }
        total = EVEN(total);
    }
    fprintf(stderr,
            "Total: header %d + contents %d + unused %d\n",
            IS_LEAF2(mp) ? PAGEHDRSZ : PAGEBASE + MP_LOWER(mp),
            total,
            SIZELEFT(mp));
}
#endif

#if (MDB_DEBUG) > 2
// Count all the pages in each DB and in the freelist
// and make sure it matches the actual number of pages
// being used.
// All named DBs must be open for a correct count.
void mdb_audit(MDB_txn* txn)
{
    MDB_cursor mc;
    MDB_val key, data;
    MDB_ID freecount, count;
    MDB_dbi i;
    int rc;

    freecount = 0;
    mdb_cursor_init(&mc, txn, FREE_DBI, NULL);
    while ((rc = mdb_cursor_get(&mc, &key, &data, MDB_NEXT)) == 0)
        freecount += *(MDB_ID*)data.mv_data;
    mdb_tassert(txn, rc == MDB_NOTFOUND);

    count = 0;
    for (i = 0; i < txn->mt_numdbs; i++)
    {
        MDB_xcursor mx;
        if (!(txn->mt_dbflags[i] & DB_VALID))
            continue;
        mdb_cursor_init(&mc, txn, i, &mx);
        if (txn->mt_dbs[i].md_root == P_INVALID)
            continue;
        count += txn->mt_dbs[i].md_branch_pages + txn->mt_dbs[i].md_leaf_pages + txn->mt_dbs[i].md_overflow_pages;
        if (txn->mt_dbs[i].md_flags & MDB_DUPSORT)
        {
            rc = mdb_page_search(&mc, NULL, MDB_PS_FIRST);
            for (; rc == MDB_SUCCESS; rc = mdb_cursor_sibling(&mc, 1))
            {
                unsigned j;
                MDB_page* mp;
                mp = mc.mc_pg[mc.mc_top];
                for (j = 0; j < NUMKEYS(mp); j++)
                {
                    MDB_node* leaf = NODEPTR(mp, j);
                    if (leaf->mn_flags & F_SUBDATA)
                    {
                        MDB_db db;
                        memcpy(&db, NODEDATA(leaf), sizeof(db));
                        count += db.md_branch_pages + db.md_leaf_pages + db.md_overflow_pages;
                    }
                }
            }
            mdb_tassert(txn, rc == MDB_NOTFOUND);
        }
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
#endif
