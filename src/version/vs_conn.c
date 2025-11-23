/*
* Tony
*  Version store connection management
*/

#include "wt_internal.h"

static int __vs_start_internal_session(WT_SESSION_IMPL *session, WT_SESSION_IMPL **int_sessionp)
{
    return (__wt_open_internal_session(S2C(session), "vs_access", true, 0, 0, int_sessionp));
}

/*
 * __vs_release_internal_session --
 *     Release the temporary internal session started to retrieve version store.
 */
static int __vs_release_internal_session(WT_SESSION_IMPL *int_session)
{
    return (__wt_session_close_internal(int_session));
}

/*
 * __wt_vs_get_btree --
 *     Get the version store btree by opening a version store cursor.
 */
int __wt_vs_get_btree(WT_SESSION_IMPL *session, WT_BTREE **vs_btreep)
{
    WT_CURSOR *vs_cursor;
    WT_DECL_RET;

    *vs_btreep = NULL;

    // Can we just use a plain file cursor here? Why not?
    WT_RET(__wt_open_cursor(session, WT_VS_URI, NULL, NULL, &vs_cursor));
    *vs_btreep = CUR2BT(vs_cursor);
    WT_ASSERT(session, *vs_btreep != NULL);
    WT_TRET(vs_cursor->close(vs_cursor));
    return (ret);
}

/*
 * __wt_vs_config --
 *     Configure the version store table.
 */
int
__wt_vs_config(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_BTREE *btree;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *tmp_setup_session;

    conn = S2C(session);
    tmp_setup_session = NULL;

    // TODO: Should add a field for version store file max size
    // Just use the history store file max for now
    WT_ERR(__wt_config_gets(session, cfg, "history_store.file_max", &cval));
    if (cval.val != 0 && cval.val < WT_VS_FILE_MIN)
        WT_ERR_MSG(session, EINVAL, "max version store size %" PRId64 " below minimum %d", cval.val,
          WT_VS_FILE_MIN);

    WT_ERR(__vs_start_internal_session(session, &tmp_setup_session));

    /*
     * Retrieve the btree from the version store cursor.
     */
    WT_ERR(__wt_vs_get_btree(tmp_setup_session, &btree));
    /* Track the version store file ID. */
    // Not used currently
    if (conn->cache->vs_fileid == 0)
        conn->cache->vs_fileid = btree->id;

    /*
     * We need to set file_max on the btree associated with one of the version store sessions.
     */
    btree->file_max = (uint64_t)cval.val;
    // WT_STAT_CONN_SET(session, cache_vs_ondisk_max, btree->file_max);

err:
    if (tmp_setup_session != NULL)
        WT_TRET(__vs_release_internal_session(tmp_setup_session));
    return (ret);
}

/*
 * __wt_vs_open --
 *     Initialize the database's version store.
 */
int
__wt_vs_open(WT_SESSION_IMPL *session, const char **cfg)
{
    // WT_CONNECTION_IMPL *conn;
    // conn = S2C(session);

    /* Create the table. */
    WT_RET(__wt_session_create(session, WT_VS_URI, WT_VS_CONFIG));

    WT_RET(__wt_vs_config(session, cfg));

    /* The statistics server is already running, make sure we don't race. */
    WT_WRITE_BARRIER();

    return (0);
}

/*
 * __wt_vs_close --
 *     Destroy the database's version store.
 */
void
__wt_vs_close(WT_SESSION_IMPL *session)
{
    WT_UNUSED(session);
    // F_CLR(S2C(session), WT_CONN_VS_OPEN);
}
