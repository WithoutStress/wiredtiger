/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __prepvs_get_files --
 *     Get list of WiredTigerPrepVS files in the home directory.
 */
static int
__prepvs_get_files(WT_SESSION_IMPL *session, char ***filesp, uint32_t *countp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    char **files, **vs_files;
    uint32_t count, i, vs_count;

    conn = S2C(session);
    files = NULL;
    vs_files = NULL;
    count = 0;
    vs_count = 0;

    /* Get directory listing from the log directory where PrepVS files are stored */
    WT_ERR(__wt_fs_directory_list(session,
        conn->log_path != NULL ? conn->log_path : "", WT_PREPVS_PREFIX, &files, &count));

    if (count == 0) {
        *filesp = NULL;
        *countp = 0;
        goto err;
    }

    /* Allocate array for VS files */
    WT_ERR(__wt_calloc_def(session, count, &vs_files));

    /* Filter PrepVS files */
    for (i = 0; i < count; i++) {
        if (strncmp(files[i], WT_PREPVS_PREFIX, strlen(WT_PREPVS_PREFIX)) == 0) {
            WT_ERR(__wt_strdup(session, files[i], &vs_files[vs_count]));
            vs_count++;
        }
    }

    *filesp = vs_files;
    *countp = vs_count;
    vs_files = NULL;

err:
    if (files != NULL)
        WT_TRET(__wt_fs_directory_list_free(session, &files, count));
    if (vs_files != NULL) {
        for (i = 0; i < vs_count; i++)
            __wt_free(session, vs_files[i]);
        __wt_free(session, vs_files);
    }
    return (ret);
}

/*
 * __prepvs_compact_process_file --
 *     Process a single WiredTigerPrepVS file and distribute entries to per-leaf LeafVS files.
 */
static int
__prepvs_compact_process_file(WT_SESSION_IMPL *session, const char *filename)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(filepath);
    WT_DECL_RET;
    WT_PREPVS_ENTRY *entries;
    WT_LEAFVS_FILE *leafvs_file;
    WT_VS_RANGE *vs_range;
    uint32_t count, i;
    uint64_t page_id;

    conn = S2C(session);
    entries = NULL;
    leafvs_file = NULL;
    vs_range = conn->vs_range;
    count = 0;

    /* Build full path to the VS file in the log directory */
    WT_RET(__wt_scr_alloc(session, 0, &filepath));
    if (conn->log_path != NULL && strlen(conn->log_path) > 0 &&
        strcmp(conn->log_path, ".") != 0)
        WT_ERR(__wt_buf_fmt(session, filepath, "%s/%s", conn->log_path, filename));
    else
        WT_ERR(__wt_buf_fmt(session, filepath, "%s", filename));

    /* Parse the PrepVS file */
    WT_ERR(__wt_prepvs_parse_file(session, filepath->data, &entries, &count));

    if (count == 0)
        goto done;

    /* Process each entry */
    for (i = 0; i < count; i++) {
        WT_PREPVS_ENTRY *entry = &entries[i];

        /* Find the leaf page for this key using vs_range */
        if (vs_range != NULL) {
            ret = __wt_vs_find_leaf_page(session, vs_range,
                entry->btree_id, &entry->key, &page_id);
            if (ret == WT_NOTFOUND) {
                /* Key not in any known range, use btree_id as page_id */
                page_id = 0;
                ret = 0;
            }
            WT_ERR(ret);
        } else {
            page_id = 0;
        }

        /* Load or create the per-leaf LeafVS file */
        ret = __wt_leafvs_file_load(session, entry->btree_id, page_id, &leafvs_file);
        if (ret == WT_NOTFOUND) {
            WT_ERR(__wt_leafvs_file_init(session, &leafvs_file, entry->btree_id, page_id));
            ret = 0;
        }
        WT_ERR(ret);

        /* Add entry to the LeafVS file */
        WT_ERR(__wt_leafvs_file_add_entry(session, leafvs_file,
            &entry->key, &entry->vid, &entry->value));

        /* Save and close the LeafVS file */
        WT_ERR(__wt_leafvs_file_save(session, leafvs_file));
        WT_ERR(__wt_leafvs_file_destroy(session, &leafvs_file));
    }

done:
    /* Delete the processed PrepVS file */
    WT_TRET(__wt_fs_remove(session, filepath->data, false));

err:
    if (leafvs_file != NULL)
        WT_TRET(__wt_leafvs_file_destroy(session, &leafvs_file));
    __wt_prepvs_entries_free(session, &entries, count);
    __wt_scr_free(session, &filepath);
    return (ret);
}

/*
 * __wt_vs_compact_check --
 *     Check if compaction is needed and perform it if threshold is exceeded.
 */
int
__wt_vs_compact_check(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    char **vs_files;
    uint32_t count, i, threshold;

    conn = S2C(session);
    vs_files = NULL;
    count = 0;

    /* Check if version store is enabled */
    if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_VERSION_STORE))
        return (0);

    threshold = conn->vs_compact_threshold;
    if (threshold == 0)
        return (0);

    /* Get list of PrepVS files */
    WT_RET(__prepvs_get_files(session, &vs_files, &count));

    /* Check if threshold is exceeded */
    if (count < threshold) {
        ret = 0;
        goto err;
    }

    /* Process each PrepVS file */
    for (i = 0; i < count; i++) {
        ret = __prepvs_compact_process_file(session, vs_files[i]);
        if (ret != 0) {
            __wt_verbose(session, WT_VERB_LOG,
                "PrepVS compaction: error processing %s: %s",
                vs_files[i], wiredtiger_strerror(ret));
            ret = 0;
        }
    }

err:
    if (vs_files != NULL) {
        for (i = 0; i < count; i++)
            __wt_free(session, vs_files[i]);
        __wt_free(session, vs_files);
    }
    return (ret);
}

/*
 * __wt_vs_compact_server --
 *     VS compaction server thread entry point.
 */
static WT_THREAD_RET
__vs_compact_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;

    session = arg;
    conn = S2C(session);

    while (FLD_ISSET(conn->log_flags, WT_CONN_LOG_VERSION_STORE)) {

        /* Wait for signal or timeout (check every second) */
        __wt_cond_wait(session, conn->vs_compact_cond, WT_MILLION, NULL);

        if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_VERSION_STORE))
            break;

        /* Check and perform compaction if needed */
        WT_ERR(__wt_vs_compact_check(session));
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "VS compact server error"));
    }
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_vs_compact_create --
 *     Start the VS compaction server thread.
 */
int
__wt_vs_compact_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /* Create condition variable */
    WT_RET(__wt_cond_alloc(session, "VS compact server", &conn->vs_compact_cond));

    /* Create the thread */
    WT_RET(__wt_thread_create(session, &conn->vs_compact_tid,
        __vs_compact_server, conn->vs_compact_session));
    conn->vs_compact_tid_set = true;

    return (0);
}

/*
 * __wt_vs_compact_destroy --
 *     Destroy the VS compaction server thread.
 */
int
__wt_vs_compact_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    /* Clear the flag first so the thread can exit its loop */
    FLD_CLR(conn->log_flags, WT_CONN_LOG_VERSION_STORE);

    if (conn->vs_compact_tid_set) {
        __wt_cond_signal(session, conn->vs_compact_cond);
        WT_TRET(__wt_thread_join(session, &conn->vs_compact_tid));
        conn->vs_compact_tid_set = false;
    }

    if (conn->vs_compact_cond != NULL)
        __wt_cond_destroy(session, &conn->vs_compact_cond);

    return (ret);
}
