/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_vs_range_init --
 *     Initialize the version store metadata structure.
 */
int
__wt_vs_range_init(WT_SESSION_IMPL *session, WT_VS_RANGE **vs_rangep)
{
    WT_DECL_RET;
    WT_VS_RANGE *vs_range;

    WT_RET(__wt_calloc_one(session, &vs_range));
    WT_ERR(__wt_rwlock_init(session, &vs_range->lock));

    vs_range->btrees = NULL;
    vs_range->btree_count = 0;
    vs_range->btree_alloc = 0;
    vs_range->checkpoint_gen = 0;

    *vs_rangep = vs_range;
    return (0);

err:
    __wt_free(session, vs_range);
    return (ret);
}

/*
 * __wt_vs_range_destroy --
 *     Destroy the version store metadata structure.
 */
int
__wt_vs_range_destroy(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range)
{
    uint32_t i, j;
    WT_VS_BTREE_RANGE *btree_meta;
    WT_VS_KEY_RANGE *range;

    if (vs_range == NULL)
        return (0);

    /* Free all btree metadata */
    for (i = 0; i < vs_range->btree_count; i++) {
        btree_meta = &vs_range->btrees[i];
        __wt_free(session, btree_meta->uri);
        
        /* Free key ranges */
        for (j = 0; j < btree_meta->range_count; j++) {
            range = &btree_meta->ranges[j];
            __wt_buf_free(session, &range->min_key);
            __wt_buf_free(session, &range->max_key);
        }
        __wt_free(session, btree_meta->ranges);
    }
    __wt_free(session, vs_range->btrees);

    __wt_rwlock_destroy(session, &vs_range->lock);
    __wt_free(session, vs_range);

    return (0);
}

/*
 * __vs_range_find_btree --
 *     Find or create btree metadata entry.
 */
static int
__vs_range_find_btree(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range,
    uint32_t btree_id, const char *uri, WT_VS_BTREE_RANGE **btree_metap, bool create)
{
    WT_VS_BTREE_RANGE *btree_meta;
    uint32_t i;

    /* Search for existing entry */
    for (i = 0; i < vs_range->btree_count; i++) {
        if (vs_range->btrees[i].btree_id == btree_id) {
            *btree_metap = &vs_range->btrees[i];
            return (0);
        }
    }

    if (!create) {
        *btree_metap = NULL;
        return (0);
    }

    /* Grow array if needed */
    if (vs_range->btree_count >= vs_range->btree_alloc) {
        uint32_t new_alloc = vs_range->btree_count + 10;
        size_t alloc_bytes = (size_t)vs_range->btree_alloc * sizeof(WT_VS_BTREE_RANGE);
        WT_RET(__wt_realloc_def(session, &alloc_bytes,
            new_alloc, &vs_range->btrees));
        vs_range->btree_alloc = new_alloc;
    }

    /* Initialize new entry */
    btree_meta = &vs_range->btrees[vs_range->btree_count++];
    memset(btree_meta, 0, sizeof(*btree_meta));
    btree_meta->btree_id = btree_id;
    if (uri != NULL)
        WT_RET(__wt_strdup(session, uri, &btree_meta->uri));

    *btree_metap = btree_meta;
    return (0);
}

/*
 * __vs_range_add_range_locked --
 *     Internal: add a key range assuming the caller already holds the write lock.
 */
static int
__vs_range_add_range_locked(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range,
    uint32_t btree_id, const char *uri, uint64_t page_id,
    WT_ITEM *min_key, WT_ITEM *max_key)
{
    WT_VS_BTREE_RANGE *btree_meta;
    WT_VS_KEY_RANGE *range;

    WT_RET(__vs_range_find_btree(session, vs_range, btree_id, uri, &btree_meta, true));

    /* Grow ranges array if needed */
    if (btree_meta->range_count >= btree_meta->range_alloc) {
        uint32_t new_alloc = btree_meta->range_count + 100;
        size_t alloc_bytes = (size_t)btree_meta->range_alloc * sizeof(WT_VS_KEY_RANGE);
        WT_RET(__wt_realloc_def(session, &alloc_bytes,
            new_alloc, &btree_meta->ranges));
        btree_meta->range_alloc = new_alloc;
    }

    /* Add new range */
    range = &btree_meta->ranges[btree_meta->range_count++];
    memset(range, 0, sizeof(*range));
    range->page_id = page_id;
    WT_RET(__wt_buf_set(session, &range->min_key, min_key->data, min_key->size));
    WT_RET(__wt_buf_set(session, &range->max_key, max_key->data, max_key->size));

    return (0);
}

/*
 * __wt_vs_range_add_range --
 *     Add a key range for a leaf page to the metadata.
 */
int
__wt_vs_range_add_range(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range,
    uint32_t btree_id, const char *uri, uint64_t page_id,
    WT_ITEM *min_key, WT_ITEM *max_key)
{
    WT_DECL_RET;

    __wt_writelock(session, &vs_range->lock);
    ret = __vs_range_add_range_locked(session, vs_range, btree_id, uri, page_id, min_key, max_key);
    __wt_writeunlock(session, &vs_range->lock);
    return (ret);
}

/*
 * __vs_range_clear_btree_locked --
 *     Internal: clear all key ranges assuming the caller already holds the write lock.
 */
static int
__vs_range_clear_btree_locked(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range, uint32_t btree_id)
{
    WT_VS_BTREE_RANGE *btree_meta;
    WT_VS_KEY_RANGE *range;
    uint32_t i;

    if (__vs_range_find_btree(session, vs_range, btree_id, NULL, &btree_meta, false) == 0 &&
        btree_meta != NULL) {
        /* Free existing key ranges */
        for (i = 0; i < btree_meta->range_count; i++) {
            range = &btree_meta->ranges[i];
            __wt_buf_free(session, &range->min_key);
            __wt_buf_free(session, &range->max_key);
        }
        btree_meta->range_count = 0;
    }

    return (0);
}

/*
 * __wt_vs_range_clear_btree --
 *     Clear all key ranges for a btree (called before rebuilding during checkpoint).
 */
int
__wt_vs_range_clear_btree(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range, uint32_t btree_id)
{
    __wt_writelock(session, &vs_range->lock);
    __vs_range_clear_btree_locked(session, vs_range, btree_id);
    __wt_writeunlock(session, &vs_range->lock);
    return (0);
}

/*
 * __vs_key_cmp --
 *     Lexicographic comparison of two WT_ITEMs.
 *     Returns negative, 0, or positive like memcmp.
 */
static int
__vs_key_cmp(const WT_ITEM *a, const WT_ITEM *b)
{
    int cmp;
    size_t min_size;

    min_size = WT_MIN(a->size, b->size);
    if (min_size > 0)
        cmp = memcmp(a->data, b->data, min_size);
    else
        cmp = 0;
    if (cmp != 0)
        return (cmp);
    return (a->size < b->size ? -1 : (a->size > b->size ? 1 : 0));
}

/*
 * __wt_vs_find_leaf_page --
 *     Find the leaf page ID for a given key using separator-based binary search.
 *     Ranges are stored with separator-based min_key boundaries in sorted order.
 *     Finds the last range whose min_key <= key (upper-bound search).
 *     Keys smaller than the first separator still route to page 0.
 */
int
__wt_vs_find_leaf_page(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range,
    uint32_t btree_id, const WT_ITEM *key, uint64_t *page_idp)
{
    WT_VS_BTREE_RANGE *btree_meta;
    WT_VS_KEY_RANGE *ranges;
    uint32_t lo, hi, mid, count;
    int cmp;

    *page_idp = 0;

    __wt_readlock(session, &vs_range->lock);

    if (__vs_range_find_btree(session, vs_range, btree_id, NULL, &btree_meta, false) != 0 ||
        btree_meta == NULL || btree_meta->range_count == 0) {
        __wt_readunlock(session, &vs_range->lock);
        return (WT_NOTFOUND);
    }

    ranges = btree_meta->ranges;
    count = btree_meta->range_count;

    /*
     * Binary search: find the largest index where min_key <= key.
     * After the loop, lo is the insertion point (upper bound).
     * The target range is at index (lo - 1), or 0 if key < all separators.
     */
    lo = 0;
    hi = count;
    while (lo < hi) {
        mid = lo + (hi - lo) / 2;
        cmp = __vs_key_cmp(key, &ranges[mid].min_key);
        if (cmp >= 0)
            lo = mid + 1;
        else
            hi = mid;
    }

    /*
     * lo == 0 means key < all separators.  Route to the first page since
     * it covers [-inf, sep[1]).  Otherwise the target is (lo - 1).
     */
    *page_idp = ranges[lo > 0 ? lo - 1 : 0].page_id;
    __wt_readunlock(session, &vs_range->lock);
    return (0);
}

/*
 * __wt_vs_range_update_on_split --
 *     Update vs_range when a page is split during reconciliation.
 *     This replaces the old page's range with multiple new ranges.
 */
int
__wt_vs_range_update_on_split(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_VS_RANGE *vs_range;

    btree = S2BT(session);
    conn = S2C(session);

    /* Only process blue: btrees with version store enabled */
    if (btree->dhandle == NULL || btree->dhandle->name == NULL ||
        strncmp(btree->dhandle->name, "blue:", 5) != 0)
        return (0);

    vs_range = conn->vs_range;
    if (vs_range == NULL)
        return (0);

    /* Only process leaf page splits (multi_next > 1) */
    if (r->multi_next <= 1)
        return (0);

    /*
     * The previous implementation cleared btree ranges and rebuilt them from split fragments
     * only, which could leave vs_range incomplete. Instead, mark a deferred rebuild request and
     * let the VS compaction thread refresh ranges and LeafVS files consistently.
     */
    ++conn->vs_leafvs_split_gen;
    if (conn->vs_compact_cond != NULL)
        __wt_cond_signal(session, conn->vs_compact_cond);

    WT_UNUSED(vs_range);
    return (0);
}

/*
 * __wt_vs_range_save --
 *     Save the version store metadata to disk.
 */
int
__wt_vs_range_save(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_FH *fh;
    WT_VS_BTREE_RANGE *btree_meta;
    WT_VS_KEY_RANGE *range;
    uint32_t i, j;
    wt_off_t offset;

    fh = NULL;

    __wt_readlock(session, &vs_range->lock);

    /* Open file for writing - use filename directly, __wt_open handles home dir */
    WT_ERR(__wt_open(session, WT_VS_RANGE_FILE, WT_FS_OPEN_FILE_TYPE_REGULAR,
        WT_FS_OPEN_CREATE, &fh));

    WT_ERR(__wt_scr_alloc(session, 4096, &buf));

    /* Write header: magic, version, btree_count, checkpoint_gen */
    WT_ERR(__wt_buf_fmt(session, buf, "%s\n%u\n%" PRIu32 "\n%" PRIu64 "\n",
        WT_VS_RANGE_MAGIC, (unsigned int)WT_VS_RANGE_VERSION,
        vs_range->btree_count, vs_range->checkpoint_gen));

    offset = 0;
    WT_ERR(__wt_write(session, fh, offset, buf->size, buf->data));
    offset += (wt_off_t)buf->size;

    /* Write each btree's metadata */
    for (i = 0; i < vs_range->btree_count; i++) {
        btree_meta = &vs_range->btrees[i];

        /* Write btree header: btree_id, uri, range_count */
        WT_ERR(__wt_buf_fmt(session, buf, "B:%u:%s:%u\n",
            btree_meta->btree_id,
            btree_meta->uri != NULL ? btree_meta->uri : "",
            btree_meta->range_count));
        WT_ERR(__wt_write(session, fh, offset, buf->size, buf->data));
        offset += (wt_off_t)buf->size;

        /* Write each key range */
        for (j = 0; j < btree_meta->range_count; j++) {
            range = &btree_meta->ranges[j];

            /* Format: R:page_id:min_key_size:max_key_size\n<min_key><max_key> */
            WT_ERR(__wt_buf_fmt(session, buf, "R:%" PRIu64 ":%zu:%zu\n",
                range->page_id, range->min_key.size, range->max_key.size));
            WT_ERR(__wt_write(session, fh, offset, buf->size, buf->data));
            offset += (wt_off_t)buf->size;

            /* Write min_key */
            if (range->min_key.size > 0) {
                WT_ERR(__wt_write(session, fh, offset,
                    range->min_key.size, range->min_key.data));
                offset += (wt_off_t)range->min_key.size;
            }

            /* Write max_key */
            if (range->max_key.size > 0) {
                WT_ERR(__wt_write(session, fh, offset,
                    range->max_key.size, range->max_key.data));
                offset += (wt_off_t)range->max_key.size;
            }
        }
    }

    /* Sync to disk */
    WT_ERR(__wt_fsync(session, fh, true));

err:
    __wt_readunlock(session, &vs_range->lock);
    if (fh != NULL)
        WT_TRET(__wt_close(session, &fh));
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_vs_range_load --
 *     Load the version store metadata from disk.
 */
int
__wt_vs_range_load(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_FH *fh;
    WT_VS_BTREE_RANGE *btree_meta;
    WT_VS_KEY_RANGE *range;
    wt_off_t file_size;
    char *line;
    char magic[8];
    uint32_t version, btree_count, range_count;
    uint32_t btree_id, i, j;
    uint64_t checkpoint_gen, page_id;
    size_t min_key_size, max_key_size;
    bool exists;

    fh = NULL;

    /* Check if file exists */
    WT_RET(__wt_fs_exist(session, WT_VS_RANGE_FILE, &exists));
    if (!exists) {
        ret = WT_NOTFOUND;
        goto err;
    }

    /* Open file for reading */
    WT_ERR(__wt_open(session, WT_VS_RANGE_FILE, WT_FS_OPEN_FILE_TYPE_REGULAR, 0, &fh));
    WT_ERR(__wt_filesize(session, fh, &file_size));

    if (file_size == 0) {
        ret = WT_NOTFOUND;
        goto err;
    }

    WT_ERR(__wt_scr_alloc(session, (size_t)file_size + 1, &buf));
    WT_ERR(__wt_read(session, fh, 0, (size_t)file_size, buf->mem));
    ((char *)buf->mem)[file_size] = '\0';

    __wt_writelock(session, &vs_range->lock);

    /* Parse header */
    line = buf->mem;
    if (sscanf(line, "%4s\n%u\n%u\n%" SCNu64 "\n",
        magic, &version, &btree_count, &checkpoint_gen) != 4 ||
        strcmp(magic, WT_VS_RANGE_MAGIC) != 0 ||
        version != WT_VS_RANGE_VERSION) {
        ret = WT_ERROR;
        goto err_unlock;
    }

    vs_range->checkpoint_gen = checkpoint_gen;

    /* Skip header lines */
    for (i = 0; i < 4 && line != NULL; i++) {
        line = strchr(line, '\n');
        if (line != NULL)
            line++;
    }

    /* Parse btree entries */
    for (i = 0; i < btree_count && line != NULL; i++) {
        char uri_buf[256];
        
        if (sscanf(line, "B:%u:%255[^:]:%u\n", &btree_id, uri_buf, &range_count) != 3) {
            ret = WT_ERROR;
            goto err_unlock;
        }

        if ((ret = __vs_range_find_btree(session, vs_range, btree_id,
            uri_buf[0] != '\0' ? uri_buf : NULL, &btree_meta, true)) != 0)
            goto err_unlock;

        /* Skip to next line */
        line = strchr(line, '\n');
        if (line != NULL)
            line++;

        /* Parse key ranges */
        for (j = 0; j < range_count && line != NULL; j++) {
            if (sscanf(line, "R:%" SCNu64 ":%zu:%zu\n",
                &page_id, &min_key_size, &max_key_size) != 3) {
                ret = WT_ERROR;
                goto err_unlock;
            }

            /* Skip to key data */
            line = strchr(line, '\n');
            if (line != NULL)
                line++;

            /* Grow ranges array if needed */
            if (btree_meta->range_count >= btree_meta->range_alloc) {
                uint32_t new_alloc = btree_meta->range_count + 100;
                size_t alloc_bytes = (size_t)btree_meta->range_alloc * sizeof(WT_VS_KEY_RANGE);
                if ((ret = __wt_realloc_def(session, &alloc_bytes,
                    new_alloc, &btree_meta->ranges)) != 0)
                    goto err_unlock;
                btree_meta->range_alloc = new_alloc;
            }

            range = &btree_meta->ranges[btree_meta->range_count++];
            memset(range, 0, sizeof(*range));
            range->page_id = page_id;

            /* Read min_key */
            if (min_key_size > 0 && line != NULL) {
                if ((ret = __wt_buf_set(session, &range->min_key, line, min_key_size)) != 0)
                    goto err_unlock;
                line += min_key_size;
            }

            /* Read max_key */
            if (max_key_size > 0 && line != NULL) {
                if ((ret = __wt_buf_set(session, &range->max_key, line, max_key_size)) != 0)
                    goto err_unlock;
                line += max_key_size;
            }
        }
    }

err_unlock:
    __wt_writeunlock(session, &vs_range->lock);
err:
    if (fh != NULL)
        WT_TRET(__wt_close(session, &fh));
    __wt_scr_free(session, &buf);
    return (ret);
}
