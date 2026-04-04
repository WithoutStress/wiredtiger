/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_LEAFVS_REBUILD_SUFFIX ".rebuild"
#define WT_LEAFVS_REBUILD_STATS_FILE "LeafVS.rebuild.stats.csv"
#define WT_VS_COMPACT_BATCH_FILES 4
#define WT_PREPVS_ROUTE_CHUNK_ENTRIES 16384

typedef struct {
    uint32_t entry_index;
    uint32_t btree_id;
    uint64_t page_id;
} WT_PREPVS_LEAFVS_ROUTE;

static int __vs_range_refresh_all(WT_SESSION_IMPL *session);

typedef struct {
    const char *filename;
    WT_VS_RANGE *vs_range;
    uint32_t total_entries;
    uint32_t total_flushed_leafvs_files;
    uint32_t chunk_count;
    uint64_t lookup_miss_total;
    uint64_t lookup_refresh_retries;
    uint64_t lookup_miss_resolved_after_refresh;
    uint64_t lookup_miss_unresolved;
    uint64_t fallback_page0_entries;
    bool lookup_refresh_retry_done;
} WT_PREPVS_COMPACT_STATE;

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
 * __leafvs_get_files --
 *     Get list of LeafVS files in the home directory.
 */
static int
__leafvs_get_files(WT_SESSION_IMPL *session, char ***filesp, uint32_t *countp)
{
    char **files;
    uint32_t count;

    files = NULL;
    count = 0;
    WT_RET(__wt_fs_directory_list(session, "", WT_LEAFVS_PREFIX, &files, &count));
    *filesp = files;
    *countp = count;
    return (0);
}

/*
 * __leafvs_count_files --
 *     Count LeafVS files and discard the listing immediately.
 */
static int
__leafvs_count_files(WT_SESSION_IMPL *session, uint32_t *countp)
{
    WT_DECL_RET;
    char **files;
    uint32_t count;

    files = NULL;
    count = 0;

    WT_ERR(__leafvs_get_files(session, &files, &count));
    *countp = count;

err:
    if (files != NULL)
        WT_TRET(__wt_fs_directory_list_free(session, &files, count));
    return (ret);
}

/*
 * __prepvs_count_files --
 *     Count PrepVS files and discard the listing immediately.
 */
static int
__prepvs_count_files(WT_SESSION_IMPL *session, uint32_t *countp)
{
    WT_DECL_RET;
    char **files;
    uint32_t count, i;

    files = NULL;
    count = 0;

    WT_ERR(__prepvs_get_files(session, &files, &count));
    *countp = count;

err:
    if (files != NULL) {
        for (i = 0; i < count; i++)
            __wt_free(session, files[i]);
        __wt_free(session, files);
    }
    return (ret);
}

/*
 * __prepvs_leafvs_route_compare --
 *     Sort PrepVS entries by destination LeafVS file while preserving input order within a page.
 */
static int
__prepvs_leafvs_route_compare(const void *a, const void *b)
{
    const WT_PREPVS_LEAFVS_ROUTE *ra, *rb;

    ra = a;
    rb = b;

    if (ra->btree_id < rb->btree_id)
        return (-1);
    if (ra->btree_id > rb->btree_id)
        return (1);
    if (ra->page_id < rb->page_id)
        return (-1);
    if (ra->page_id > rb->page_id)
        return (1);
    if (ra->entry_index < rb->entry_index)
        return (-1);
    if (ra->entry_index > rb->entry_index)
        return (1);
    return (0);
}

/*
 * __vs_route_entries_chunk --
 *     Route and flush one bounded chunk of version-store entries into LeafVS files.
 */
static int
__vs_route_entries_chunk(
  WT_SESSION_IMPL *session, WT_PREPVS_ENTRY *entries, uint32_t count, void *cookie)
{
    WT_PREPVS_COMPACT_STATE *state;
    WT_DECL_RET;
    WT_LEAFVS_FILE *leafvs_file;
    WT_PREPVS_LEAFVS_ROUTE *routes;
    uint32_t chunk_lookup_miss, chunk_lookup_miss_resolved, chunk_lookup_miss_unresolved;
    uint32_t chunk_refresh_retries;
    uint32_t current_btree_id, flushed_leafvs_files, i;
    uint64_t current_page_id, page_id;
    bool leafvs_open;

    state = cookie;
    leafvs_file = NULL;
    routes = NULL;
    current_btree_id = 0;
    current_page_id = 0;
    chunk_lookup_miss = 0;
    chunk_lookup_miss_resolved = 0;
    chunk_lookup_miss_unresolved = 0;
    chunk_refresh_retries = 0;
    flushed_leafvs_files = 0;
    leafvs_open = false;

    WT_ERR(__wt_calloc_def(session, count, &routes));

    for (i = 0; i < count; i++) {
        WT_PREPVS_ENTRY *entry;

        entry = &entries[i];
        if (state->vs_range != NULL) {
            ret = __wt_vs_find_leaf_page(session, state->vs_range, entry->btree_id, &entry->key, &page_id);
            if (ret == WT_NOTFOUND) {
                ++chunk_lookup_miss;
                ++state->lookup_miss_total;

                if (!state->lookup_refresh_retry_done) {
                    printf("VS compact server: refreshing vs_range after lookup miss in %s at chunk entry %u/%u\n",
                      state->filename, i + 1, count);
                    fflush(stdout);
                    WT_ERR(__vs_range_refresh_all(session));
                    state->lookup_refresh_retry_done = true;
                    ++state->lookup_refresh_retries;
                    ++chunk_refresh_retries;
                    ret = __wt_vs_find_leaf_page(
                      session, state->vs_range, entry->btree_id, &entry->key, &page_id);
                    if (ret == 0) {
                        ++state->lookup_miss_resolved_after_refresh;
                        ++chunk_lookup_miss_resolved;
                    }
                }

                if (ret == WT_NOTFOUND) {
                    ++state->lookup_miss_unresolved;
                    ++chunk_lookup_miss_unresolved;
                    ++state->fallback_page0_entries;
                    page_id = 0;
                    ret = 0;
                }
            }
            if (ret != 0) {
                printf("VS compact server: leaf lookup failed for %s at chunk entry %u/%u, btree=%u, ret=%d\n",
                  state->filename, i + 1, count, entry->btree_id, ret);
                fflush(stdout);
            }
            WT_ERR(ret);
        } else
            page_id = 0;

        routes[i].entry_index = i;
        routes[i].btree_id = entry->btree_id;
        routes[i].page_id = page_id;
    }

    __wt_qsort(routes, count, sizeof(WT_PREPVS_LEAFVS_ROUTE), __prepvs_leafvs_route_compare);

    for (i = 0; i < count; i++) {
        WT_PREPVS_ENTRY *entry;
        WT_PREPVS_LEAFVS_ROUTE *route;
        uint32_t total_processed;

        route = &routes[i];
        entry = &entries[route->entry_index];

        if (!leafvs_open || current_btree_id != route->btree_id || current_page_id != route->page_id) {
            if (leafvs_open) {
                WT_ERR(__wt_leafvs_file_save(session, leafvs_file));
                WT_ERR(__wt_leafvs_file_destroy(session, &leafvs_file));
                leafvs_open = false;
                flushed_leafvs_files++;
            }

            ret = __wt_leafvs_file_load(session, route->btree_id, route->page_id, &leafvs_file);
            if (ret == WT_NOTFOUND) {
                WT_ERR(__wt_leafvs_file_init(session, &leafvs_file, route->btree_id, route->page_id));
                ret = 0;
            }
            if (ret != 0) {
                printf("VS compact server: LeafVS load/init failed for %s at chunk entry %u/%u, btree=%u, page=%llu, ret=%d\n",
                  state->filename, i + 1, count, route->btree_id, (unsigned long long)route->page_id,
                  ret);
                fflush(stdout);
            }
            WT_ERR(ret);

            current_btree_id = route->btree_id;
            current_page_id = route->page_id;
            leafvs_open = true;
        }

        ret = __wt_leafvs_file_add_entry(session, leafvs_file, &entry->key, &entry->vid, &entry->value);
        if (ret != 0) {
            printf("VS compact server: LeafVS add failed for %s at chunk entry %u/%u, btree=%u, page=%llu, ret=%d\n",
              state->filename, i + 1, count, route->btree_id, (unsigned long long)route->page_id, ret);
            fflush(stdout);
        }
        WT_ERR(ret);

        total_processed = state->total_entries + i + 1;
        if (total_processed == 1 || total_processed % 10000 == 0 || i + 1 == count) {
            printf("VS compact server: progress %s %u entries processed\n", state->filename, total_processed);
            fflush(stdout);
        }
    }

    if (leafvs_open) {
        WT_ERR(__wt_leafvs_file_save(session, leafvs_file));
        WT_ERR(__wt_leafvs_file_destroy(session, &leafvs_file));
        leafvs_open = false;
        flushed_leafvs_files++;
    }

    state->total_entries += count;
    state->total_flushed_leafvs_files += flushed_leafvs_files;
    state->chunk_count++;
    if (chunk_lookup_miss > 0) {
        printf("VS compact server: lookup miss summary for %s chunk %u: misses=%u, "
          "refresh_retries=%u, resolved_after_refresh=%u, unresolved=%u, fallback_page0=%u, "
          "cumulative_misses=%llu\n",
          state->filename, state->chunk_count, chunk_lookup_miss, chunk_refresh_retries,
          chunk_lookup_miss_resolved, chunk_lookup_miss_unresolved, chunk_lookup_miss_unresolved,
          (unsigned long long)state->lookup_miss_total);
        fflush(stdout);
    }
    printf("VS compact server: completed chunk %u for %s (%u entries, %u LeafVS files, cumulative=%u)\n",
      state->chunk_count, state->filename, count, flushed_leafvs_files, state->total_entries);
    fflush(stdout);

err:
    if (leafvs_file != NULL)
        WT_TRET(__wt_leafvs_file_destroy(session, &leafvs_file));
    __wt_free(session, routes);
    return (ret);
}

/*
 * __leafvs_parse_filename --
 *     Parse LeafVS_<btree_id>_<page_id>.vs filename.
 */
static int
__leafvs_parse_filename(WT_SESSION_IMPL *session, const char *filename,
  uint32_t *btree_idp, uint64_t *page_idp)
{
    unsigned int btree_id;
    unsigned long long page_id;

    btree_id = 0;
    page_id = 0;
    if (sscanf(filename, WT_LEAFVS_PREFIX "%u_%llu" WT_LEAFVS_SUFFIX, &btree_id, &page_id) != 2 &&
      sscanf(filename, WT_LEAFVS_PREFIX "%u_%llu" WT_LEAFVS_SUFFIX WT_LEAFVS_REBUILD_SUFFIX,
        &btree_id, &page_id) != 2)
        WT_RET_MSG(session, EINVAL, "Invalid LeafVS filename: %s", filename);

    *btree_idp = (uint32_t)btree_id;
    *page_idp = (uint64_t)page_id;
    return (0);
}

/*
 * __leafvs_make_rebuild_name --
 *     Build "<leafvs>.rebuild" temporary source name.
 */
static int
__leafvs_make_rebuild_name(WT_SESSION_IMPL *session, const char *name, WT_ITEM *tmp_name)
{
    WT_RET(__wt_buf_fmt(session, tmp_name, "%s%s", name, WT_LEAFVS_REBUILD_SUFFIX));
    return (0);
}

/*
 * __leafvs_read_header --
 *     Read and validate a LeafVS file header.
 */
[[maybe_unused]] static int
__leafvs_read_header(
  WT_SESSION_IMPL *session, const char *filename, WT_LEAFVS_FILE_HEADER *headerp)
{
    WT_DECL_RET;
    WT_FH *fh;

    fh = NULL;
    WT_ERR(__wt_open(session, filename, WT_FS_OPEN_FILE_TYPE_REGULAR, 0, &fh));
    WT_ERR(__wt_read(session, fh, (wt_off_t)0, sizeof(*headerp), headerp));

    if (memcmp(headerp->magic, WT_LEAFVS_MAGIC, 4) != 0 ||
      headerp->version != WT_LEAFVS_FORMAT_VERSION)
        WT_ERR_MSG(session, WT_ERROR, "Invalid LeafVS header: %s", filename);

err:
    if (fh != NULL)
        WT_TRET(__wt_close(session, &fh));
    return (ret);
}

/*
 * __leafvs_dump_final_stats --
 *     Dump per-leaf LeafVS stats after rebuild for debugging.
 */
[[maybe_unused]] static int
__leafvs_dump_final_stats(WT_SESSION_IMPL *session)
{
    WT_DECL_ITEM(path);
    WT_DECL_RET;
    WT_FSTREAM *fs;
    WT_LEAFVS_FILE_HEADER header;
    char **leafvs_files;
    uint32_t btree_id, count, i, dumped;
    uint64_t page_id;
    uint64_t total_versions, total_bytes, max_versions, max_bytes;

    path = NULL;
    fs = NULL;
    leafvs_files = NULL;
    count = 0;
    dumped = 0;
    total_versions = 0;
    total_bytes = 0;
    max_versions = 0;
    max_bytes = 0;

    WT_RET(__leafvs_get_files(session, &leafvs_files, &count));
    if (count == 0)
        return (0);

    WT_ERR(__wt_scr_alloc(session, 0, &path));
    WT_ERR(__wt_buf_fmt(session, path, "%s", WT_LEAFVS_REBUILD_STATS_FILE));
    WT_ERR(__wt_fopen(
      session, (const char *)path->data, WT_FS_OPEN_CREATE | WT_FS_OPEN_FIXED, WT_STREAM_WRITE, &fs));
    WT_ERR(__wt_fprintf(session, fs, "file,btree_id,page_id,key_count,version_count,data_size\n"));

    for (i = 0; i < count; ++i) {
        const char *name;
        size_t nlen, slen;

        name = leafvs_files[i];
        nlen = strlen(name);
        slen = strlen(WT_LEAFVS_SUFFIX);
        if (nlen < slen || strcmp(name + (nlen - slen), WT_LEAFVS_SUFFIX) != 0)
            continue;

        WT_ERR(__leafvs_parse_filename(session, name, &btree_id, &page_id));
        WT_ERR(__leafvs_read_header(session, name, &header));
        WT_ERR(__wt_fprintf(session, fs, "%s,%u,%llu,%llu,%llu,%llu\n", name, btree_id,
          (unsigned long long)page_id, (unsigned long long)header.key_count,
          (unsigned long long)header.version_count, (unsigned long long)header.data_size));

        total_versions += header.version_count;
        total_bytes += header.data_size;
        if (header.version_count > max_versions)
            max_versions = header.version_count;
        if (header.data_size > max_bytes)
            max_bytes = header.data_size;
        dumped++;
    }

    WT_ERR(__wt_fflush(session, fs));
    __wt_verbose(session, WT_VERB_LOG,
      "LeafVS rebuild stats dumped: file=%s leaves=%u total_versions=%llu total_bytes=%llu "
      "max_versions=%llu max_bytes=%llu",
      WT_LEAFVS_REBUILD_STATS_FILE, dumped, (unsigned long long)total_versions,
      (unsigned long long)total_bytes, (unsigned long long)max_versions,
      (unsigned long long)max_bytes);

err:
    WT_TRET(__wt_fclose(session, &fs));
    if (leafvs_files != NULL)
        WT_TRET(__wt_fs_directory_list_free(session, &leafvs_files, count));
    __wt_scr_free(session, &path);
    return (ret);
}

/*
 * __vs_collect_leaf_seps_recurse --
 *     Recursively walk internal pages and collect separator-based ranges for
 *     leaf children.  For each leaf child encountered in DFS order, the
 *     previous leaf's range is closed with the current separator as its upper
 *     bound.  Leaf pages are never read; only internal-page separator keys
 *     (already cached) are accessed.
 */
static int
__vs_collect_leaf_seps_recurse(WT_SESSION_IMPL *session, WT_PAGE *intl_page,
  WT_VS_RANGE *vs_range, uint32_t btree_id, const char *uri,
  uint64_t *page_idp, WT_ITEM *prev_sep)
{
    WT_DECL_RET;
    WT_PAGE_INDEX *pindex;
    WT_REF *ref;
    uint32_t i;
    void *key;
    size_t size;

    WT_INTL_INDEX_GET(session, intl_page, pindex);

    for (i = 0; i < pindex->entries; i++) {
        ref = pindex->index[i];

        if (F_ISSET(ref, WT_REF_FLAG_LEAF)) {
            /*
             * Get the separator key for this leaf child from the parent
             * internal page.  If a previous leaf exists, emit its range
             * with this separator as the upper bound.
             */
            __wt_ref_key(intl_page, ref, &key, &size);

            if (*page_idp > 0) {
                WT_ITEM max_key;
                max_key.data = key;
                max_key.size = size;
                WT_RET(__wt_vs_range_add_range(session, vs_range, btree_id, uri,
                  *page_idp - 1, prev_sep, &max_key));
            }

            /* Record this separator as the start of the next range. */
            WT_RET(__wt_buf_set(session, prev_sep, key, size));
            (*page_idp)++;
        } else {
            /* Internal child: read page into cache, recurse, release. */
            WT_RET(__wt_page_in(session, ref,
              WT_READ_NO_GEN | WT_READ_WONT_NEED));
            ret = __vs_collect_leaf_seps_recurse(session, ref->page,
              vs_range, btree_id, uri, page_idp, prev_sep);
            WT_TRET(__wt_page_release(session, ref, 0));
            WT_RET(ret);
        }
    }

    return (0);
}

/*
 * __vs_range_refresh_one_btree --
 *     Rebuild key ranges for one btree URI using internal-page separator keys.
 *     Walks only internal pages (never reads leaf pages) to collect the
 *     separator keys that define each leaf page's key coverage range.
 *     Complexity is O(I) where I = number of internal-page entries, which
 *     equals the number of leaf pages L.  No leaf-page I/O is performed.
 */
static int
__vs_range_refresh_one_btree(WT_SESSION_IMPL *session, WT_VS_RANGE *vs_range,
  const char *uri)
{
    WT_BTREE *btree;
    WT_CURSOR *cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DATA_HANDLE *saved_dhandle;
    WT_DECL_ITEM(prev_sep);
    WT_DECL_RET;
    WT_ITEM sentinel;
    WT_PAGE *root_page;
    WT_SESSION *wt_session;
    uint32_t btree_id;
    uint64_t page_id;

    cursor = NULL;
    page_id = 0;
    saved_dhandle = NULL;
    wt_session = (WT_SESSION *)session;
    btree_id = 0;

    WT_RET(__wt_scr_alloc(session, 0, &prev_sep));

    /* Open a cursor to obtain the btree handle and btree_id. */
    WT_ERR(wt_session->open_cursor(wt_session, uri, NULL, NULL, &cursor));
    cbt = (WT_CURSOR_BTREE *)cursor;
    btree = CUR2BT(cbt);
    btree_id = btree->id;

    WT_ERR(__wt_vs_range_clear_btree(session, vs_range, btree_id));

    /*
     * Set session->dhandle so internal-page access finds the correct btree.
     * Save and restore the previous dhandle around our walk.
     */
    saved_dhandle = session->dhandle;
    session->dhandle = cbt->dhandle;

    root_page = btree->root.page;
    if (root_page == NULL)
        goto done;

    if (root_page->type == WT_PAGE_ROW_LEAF) {
        /*
         * Single-page tree: one range covering the entire key space.
         * Use empty min/max keys to represent [-inf, +inf].
         */
        sentinel.data = "";
        sentinel.size = 0;
        WT_ERR(__wt_vs_range_add_range(
          session, vs_range, btree_id, uri, 0, &sentinel, &sentinel));
        page_id = 1;
    } else {
        /*
         * Walk internal pages recursively collecting separator keys.
         * Protect against concurrent splits with the split generation.
         */
        WT_ENTER_PAGE_INDEX(session);
        ret = __vs_collect_leaf_seps_recurse(session, root_page,
          vs_range, btree_id, uri, &page_id, prev_sep);
        WT_LEAVE_PAGE_INDEX(session);
        WT_ERR(ret);

        /* Emit the last leaf's range: [last_sep, +inf). */
        if (page_id > 0) {
            sentinel.data = "";
            sentinel.size = 0;
            WT_ERR(__wt_vs_range_add_range(session, vs_range, btree_id, uri,
              page_id - 1, prev_sep, &sentinel));
        }
    }

done:
err:
    if (saved_dhandle != NULL)
        session->dhandle = saved_dhandle;
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    __wt_scr_free(session, &prev_sep);
    return (ret);
}

/*
 * __vs_range_refresh_all --
 *     Refresh all registered btree ranges from current tree shape.
 */
static int
__vs_range_refresh_all(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *meta_cursor;
    WT_DECL_RET;
    uint64_t time_start, time_stop;
    WT_SESSION *wt_session;
    WT_VS_RANGE *vs_range;
    const char *uri;
    char **uris;
    size_t uris_alloc;
    uint32_t count, i;

    conn = S2C(session);
    wt_session = (WT_SESSION *)session;
    vs_range = conn->vs_range;
    meta_cursor = NULL;
    uris = NULL;
    uris_alloc = 0;
    count = 0;
    time_start = __wt_clock(session);

    if (vs_range == NULL)
        return (0);

    WT_ERR(wt_session->open_cursor(wt_session, "metadata:", NULL, NULL, &meta_cursor));
    while ((ret = meta_cursor->next(meta_cursor)) == 0) {
        WT_ERR(meta_cursor->get_key(meta_cursor, &uri));
        if (uri == NULL || strncmp(uri, "blue:", 5) != 0)
            continue;
        WT_ERR(__wt_realloc_def(session, &uris_alloc, count + 1, &uris));
        WT_ERR(__wt_strdup(session, uri, &uris[count]));
        count++;
    }
    if (ret == WT_NOTFOUND)
        ret = 0;
    WT_ERR(ret);

    for (i = 0; i < count; ++i)
        WT_ERR(__vs_range_refresh_one_btree(session, vs_range, uris[i]));

    __wt_writelock(session, &vs_range->lock);
    vs_range->checkpoint_gen++;
    __wt_writeunlock(session, &vs_range->lock);
    WT_ERR(__wt_vs_range_save(session, vs_range));
    time_stop = __wt_clock(session);
    printf("VS compact server: refreshed vs_range for %u blue trees in %" PRIu64 " ms\n",
      count, WT_CLOCKDIFF_MS(time_stop, time_start));
    fflush(stdout);

err:
    if (meta_cursor != NULL)
        WT_TRET(meta_cursor->close(meta_cursor));
    if (uris != NULL) {
        for (i = 0; i < count; ++i)
            __wt_free(session, uris[i]);
        __wt_free(session, uris);
    }
    return (ret);
}

/*
 * __leafvs_rebuild_file --
 *     Rebuild one LeafVS file by redistributing its entries with current vs_range mapping.
 */
static int
__leafvs_rebuild_file(WT_SESSION_IMPL *session, const char *filename)
{
    WT_CONNECTION_IMPL *conn;
    WT_PREPVS_COMPACT_STATE compact_state;
    WT_DECL_RET;
    WT_LEAFVS_FILE *src_file;
    WT_PREPVS_ENTRY *entries;
    WT_LEAFVS_KEY_GROUP *group;
    WT_LEAFVS_VER *ver;
    uint32_t count, i, j;

    conn = S2C(session);
    src_file = NULL;
    entries = NULL;
    count = 0;
    memset(&compact_state, 0, sizeof(compact_state));
    compact_state.filename = filename;
    compact_state.vs_range = conn->vs_range;

    WT_ERR(__wt_leafvs_file_load_path(session, filename, &src_file));
    WT_ERR(__wt_calloc_def(session, WT_PREPVS_ROUTE_CHUNK_ENTRIES, &entries));

    for (i = 0; i < src_file->group_count; ++i) {
        group = &src_file->groups[i];
        for (j = 0; j < group->version_count; ++j) {
            WT_PREPVS_ENTRY *entry;

            ver = &group->versions[j];
            entry = &entries[count];
            memset(entry, 0, sizeof(*entry));
            entry->btree_id = src_file->btree_id;
            WT_ERR(__wt_buf_set(session, &entry->key, group->key.data, group->key.size));
            WT_ERR(__wt_buf_set(session, &entry->vid, ver->vid.data, ver->vid.size));
            WT_ERR(__wt_buf_set(session, &entry->value, ver->value.data, ver->value.size));
            count++;

            if (count == WT_PREPVS_ROUTE_CHUNK_ENTRIES) {
                WT_ERR(__vs_route_entries_chunk(session, entries, count, &compact_state));
                __wt_prepvs_entries_free(session, &entries, count);
                count = 0;
                WT_ERR(__wt_calloc_def(session, WT_PREPVS_ROUTE_CHUNK_ENTRIES, &entries));
            }
        }
    }

    if (count > 0) {
        WT_ERR(__vs_route_entries_chunk(session, entries, count, &compact_state));
        __wt_prepvs_entries_free(session, &entries, count);
        count = 0;
    }

    printf("VS rebuild: routed %u entries from %s across %u chunks into %u LeafVS files\n",
      compact_state.total_entries, filename, compact_state.chunk_count,
      compact_state.total_flushed_leafvs_files);
    fflush(stdout);

    /* Always remove the temporary source file once redistribution is complete. */
    WT_TRET(__wt_fs_remove(session, filename, false));

err:
    if (entries != NULL)
        __wt_prepvs_entries_free(session, &entries, count);
    if (src_file != NULL)
        WT_TRET(__wt_leafvs_file_destroy(session, &src_file));
    return (ret);
}

/*
 * __wt_leafvs_rebuild_all --
 *     Rebuild all LeafVS files using current vs_range mapping.
 */
static int
__wt_leafvs_rebuild_all(WT_SESSION_IMPL *session)
{
    WT_DECL_ITEM(tmp_name);
    WT_DECL_RET;
    char **rebuild_sources;
    char **leafvs_files;
    size_t rebuild_alloc;
    uint32_t count, i, rebuild_count;

    tmp_name = NULL;
    rebuild_sources = NULL;
    leafvs_files = NULL;
    count = 0;
    rebuild_alloc = 0;
    rebuild_count = 0;

    WT_RET(__leafvs_get_files(session, &leafvs_files, &count));
    if (count == 0)
        return (0);

    WT_ERR(__wt_scr_alloc(session, 0, &tmp_name));

    /*
     * Phase 1: move all source LeafVS files to ".rebuild" names so destination namespace starts
     * empty and we don't accumulate duplicates while redistributing.
     */
    for (i = 0; i < count; ++i) {
        const char *name;
        size_t nlen, slen;

        name = leafvs_files[i];
        nlen = strlen(name);
        slen = strlen(WT_LEAFVS_SUFFIX);
        if (nlen < slen || strcmp(name + (nlen - slen), WT_LEAFVS_SUFFIX) != 0)
            continue;

        WT_ERR(__leafvs_make_rebuild_name(session, name, tmp_name));
        WT_ERR(__wt_fs_rename(session, name, tmp_name->data, false));
        WT_ERR(__wt_realloc_def(session, &rebuild_alloc, rebuild_count + 1, &rebuild_sources));
        WT_ERR(__wt_strdup(session, (const char *)tmp_name->data, &rebuild_sources[rebuild_count]));
        rebuild_count++;
    }

    WT_TRET(__wt_fs_directory_list_free(session, &leafvs_files, count));
    leafvs_files = NULL;
    count = 0;

    /* Phase 2: redistribute from temporary sources into fresh LeafVS files. */
    for (i = 0; i < rebuild_count; ++i) {
        ret = __leafvs_rebuild_file(session, rebuild_sources[i]);
        if (ret != 0) {
            __wt_verbose(session, WT_VERB_LOG,
              "LeafVS rebuild: error processing %s: %s",
              rebuild_sources[i], wiredtiger_strerror(ret));
            ret = 0;
        }
    }

    // WT_ERR(__leafvs_dump_final_stats(session));

err:
    if (leafvs_files != NULL)
        WT_TRET(__wt_fs_directory_list_free(session, &leafvs_files, count));
    __wt_scr_free(session, &tmp_name);
    if (rebuild_sources != NULL) {
        for (i = 0; i < rebuild_count; ++i)
            __wt_free(session, rebuild_sources[i]);
        __wt_free(session, rebuild_sources);
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
    WT_PREPVS_COMPACT_STATE compact_state;
    WT_DECL_ITEM(filepath);
    WT_DECL_RET;
    uint64_t time_start, time_stop;
    uint32_t count;

    conn = S2C(session);
    count = 0;
    memset(&compact_state, 0, sizeof(compact_state));
    compact_state.filename = filename;
    compact_state.vs_range = conn->vs_range;
    time_start = __wt_clock(session);

    /* Build full path to the VS file in the log directory */
    WT_RET(__wt_scr_alloc(session, 0, &filepath));
    if (conn->log_path != NULL && strlen(conn->log_path) > 0 &&
        strcmp(conn->log_path, ".") != 0)
        WT_ERR(__wt_buf_fmt(session, filepath, "%s/%s", conn->log_path, filename));
    else
        WT_ERR(__wt_buf_fmt(session, filepath, "%s", filename));

    printf("VS compact server: begin processing %s\n", (const char *)filepath->data);
    fflush(stdout);

    WT_ERR(__wt_prepvs_parse_file_chunked(session, filepath->data, WT_PREPVS_ROUTE_CHUNK_ENTRIES,
      __vs_route_entries_chunk, &compact_state, &count));

    printf("VS compact server: parsed %u entries from %s across %u chunks\n",
      count, filename, compact_state.chunk_count);
    printf("VS compact server: flushed %u LeafVS files for %s\n",
      compact_state.total_flushed_leafvs_files, filename);
    if (compact_state.lookup_miss_total > 0)
        printf("VS compact server: lookup miss summary for %s: misses=%llu, refresh_retries=%llu, "
          "resolved_after_refresh=%llu, unresolved=%llu, fallback_page0=%llu\n",
          filename, (unsigned long long)compact_state.lookup_miss_total,
          (unsigned long long)compact_state.lookup_refresh_retries,
          (unsigned long long)compact_state.lookup_miss_resolved_after_refresh,
          (unsigned long long)compact_state.lookup_miss_unresolved,
          (unsigned long long)compact_state.fallback_page0_entries);
    fflush(stdout);

    /* Delete the processed PrepVS file */
    printf("VS compact server: removing processed PrepVS %s\n", filename);
    fflush(stdout);
    WT_TRET(__wt_fs_remove(session, filepath->data, false));
    if (ret == 0) {
        time_stop = __wt_clock(session);
        printf("VS compact server: finished %s (%u entries)\n", filename, count);
        printf("VS compact server: file summary %s duration_ms=%" PRIu64 ", entries=%u, leafvs_files=%u\n",
          filename, WT_CLOCKDIFF_MS(time_stop, time_start), count,
          compact_state.total_flushed_leafvs_files);
        fflush(stdout);
    }

err:
    if (ret != 0) {
        printf("VS compact server: failed %s after %u entries across %u chunks, ret=%d\n",
          filename, compact_state.total_entries, compact_state.chunk_count, ret);
        fflush(stdout);
    }
    __wt_scr_free(session, &filepath);
    return (ret);
}

/*
 * __wt_vs_compact_check --
 *     Check if compaction is needed and perform it if threshold is exceeded.
 */
static int
__vs_compact_check_int(WT_SESSION_IMPL *session, uint32_t max_files, bool *more_workp,
  uint32_t *found_countp, uint32_t *processed_countp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    char **vs_files;
    uint32_t count, i, process_count, threshold;

    conn = S2C(session);
    vs_files = NULL;
    count = 0;
    process_count = 0;
    if (more_workp != NULL)
        *more_workp = false;
    if (found_countp != NULL)
        *found_countp = 0;
    if (processed_countp != NULL)
        *processed_countp = 0;

    /* Check if version store is enabled */
    if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_VERSION_STORE))
        return (0);

    threshold = conn->vs_compact_threshold;
    if (threshold == 0)
        return (0);

    /* Get list of PrepVS files */
    WT_RET(__prepvs_get_files(session, &vs_files, &count));
    if (found_countp != NULL)
        *found_countp = count;

    if (count > 0) {
        printf("VS compact server: found %u PrepVS files (threshold=%u)\n", count, threshold);
        fflush(stdout);
    }

    /* Check if threshold is exceeded */
    if (count < threshold) {
        ret = 0;
        goto err;
    }

    process_count = max_files == 0 ? count : WT_MIN(count, max_files);
    if (processed_countp != NULL)
        *processed_countp = process_count;
    if (more_workp != NULL && count > process_count)
        *more_workp = true;

    /* Process each PrepVS file */
    for (i = 0; i < process_count; i++) {
        printf("VS compact server: processing PrepVS file %u, %s\n", i, vs_files[i]);
        ret = __prepvs_compact_process_file(session, vs_files[i]);
        if (ret != 0) {
            printf("VS compact server: error processing %s, ret=%d\n", vs_files[i], ret);
            fflush(stdout);
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

int
__wt_vs_compact_check(WT_SESSION_IMPL *session)
{
    return (__vs_compact_check_int(session, 0, NULL, NULL, NULL));
}

/*
 * __wt_vs_compact_remaining --
 *     Compact all remaining PrepVS files regardless of threshold. Kept for explicit drain/bounded
 *     policies and debugging.
 */
int
__wt_vs_compact_remaining(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_CONNECTION_IMPL *conn;
    char **vs_files;
    uint32_t count, i;

    conn = S2C(session);
    vs_files = NULL;
    count = 0;

    if (conn->vs_range != NULL) {
        WT_RET(__vs_range_refresh_all(session));
        WT_RET(__wt_leafvs_rebuild_all(session));
    }

    /* Get list of remaining PrepVS files */
    WT_RET(__prepvs_get_files(session, &vs_files, &count));

    if (count == 0)
        return (0);

    printf("VS compact close: processing %u remaining PrepVS files\n", count);
    fflush(stdout);
    __wt_verbose(session, WT_VERB_LOG,
        "PrepVS compaction on close: processing %u remaining file(s)", (unsigned int)count);

    /* Process each remaining PrepVS file */
    for (i = 0; i < count; i++) {
        printf("VS compact close: processing PrepVS file %u, %s\n", i, vs_files[i]);
        fflush(stdout);
        ret = __prepvs_compact_process_file(session, vs_files[i]);
        if (ret != 0) {
            printf("VS compact close: error processing %s, ret=%d\n", vs_files[i], ret);
            fflush(stdout);
            __wt_verbose(session, WT_VERB_LOG,
                "PrepVS compaction on close: error processing %s: %s",
                vs_files[i], wiredtiger_strerror(ret));
            ret = 0;
        }
    }

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
    bool immediate_retry, more_prepvs_work;
    WT_SESSION_IMPL *session;
    uint32_t leafvs_count, prepvs_count, processed_prepvs_files, visible_prepvs_files;
    uint64_t cycle_ms, cycle_start, cycle_stop, rebuild_gen, rebuild_ms, rebuild_start, rebuild_stop;
    bool did_compact_work, did_range_refresh, did_rebuild_work;

    session = arg;
    conn = S2C(session);
    immediate_retry = false;

    while (FLD_ISSET(conn->log_flags, WT_CONN_LOG_VERSION_STORE)) {
        cycle_start = __wt_clock(session);
        cycle_ms = 0;
        rebuild_ms = 0;
        did_compact_work = false;
        did_range_refresh = false;
        did_rebuild_work = false;
        visible_prepvs_files = 0;
        processed_prepvs_files = 0;

        /* Wait for signal or timeout (check every second) unless we're draining queued work. */
        if (!immediate_retry)
            __wt_cond_wait(session, conn->vs_compact_cond, WT_MILLION, NULL);
        immediate_retry = false;

        if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_VERSION_STORE))
            break;

        rebuild_gen = conn->vs_leafvs_split_gen;
        if (conn->vs_leafvs_rebuild_gen != rebuild_gen) {
            bool rebuild_completed;

            rebuild_completed = true;
            printf("VS compact server: refreshing VS ranges for split generation %" PRIu64 "\n",
              rebuild_gen);
            fflush(stdout);
            rebuild_start = __wt_clock(session);
            WT_ERR(__vs_range_refresh_all(session));
            did_range_refresh = true;
            WT_ERR(__leafvs_count_files(session, &leafvs_count));
            if (leafvs_count == 0) {
                printf("VS compact server: skipping LeafVS rebuild for split generation %" PRIu64
                  " because no LeafVS files exist yet\n",
                  rebuild_gen);
                fflush(stdout);
            } else {
                WT_ERR(__prepvs_count_files(session, &prepvs_count));
                if (prepvs_count >= conn->vs_compact_threshold && prepvs_count > 0) {
                    printf("VS compact server: deferring rebuild of %u LeafVS files for split generation %" PRIu64
                      " because %u PrepVS files are still pending\n",
                      leafvs_count, rebuild_gen, prepvs_count);
                    fflush(stdout);
                    rebuild_completed = false;
                } else {
                    printf("VS compact server: rebuilding %u LeafVS files for split generation %" PRIu64
                      "\n",
                      leafvs_count, rebuild_gen);
                    fflush(stdout);
                    WT_ERR(__wt_leafvs_rebuild_all(session));
                    did_rebuild_work = true;
                }
            }
            rebuild_stop = __wt_clock(session);
            rebuild_ms = WT_CLOCKDIFF_MS(rebuild_stop, rebuild_start);
            if (rebuild_completed)
                conn->vs_leafvs_rebuild_gen = rebuild_gen;
        }

        /* Check and perform compaction in batches so split-driven rebuilds are not starved. */
        more_prepvs_work = false;
        WT_ERR(__vs_compact_check_int(session, WT_VS_COMPACT_BATCH_FILES, &more_prepvs_work,
          &visible_prepvs_files, &processed_prepvs_files));
        did_compact_work = processed_prepvs_files > 0;
        cycle_stop = __wt_clock(session);
        cycle_ms = WT_CLOCKDIFF_MS(cycle_stop, cycle_start);
        if (did_range_refresh || did_rebuild_work || did_compact_work || cycle_ms >= 1000) {
            printf("VS compact server: cycle summary split_gen=%" PRIu64
              " rebuild_gen=%" PRIu64 " prepvs_visible=%u prepvs_processed=%u more_work=%s "
              "range_refresh=%s rebuild=%s rebuild_ms=%" PRIu64 " cycle_ms=%" PRIu64 "\n",
              conn->vs_leafvs_split_gen, conn->vs_leafvs_rebuild_gen, visible_prepvs_files,
              processed_prepvs_files, more_prepvs_work ? "true" : "false",
              did_range_refresh ? "true" : "false", did_rebuild_work ? "true" : "false",
              rebuild_ms, cycle_ms);
            fflush(stdout);
        }
        if (conn->vs_leafvs_rebuild_gen != conn->vs_leafvs_split_gen || more_prepvs_work)
            immediate_retry = true;
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
    conn->vs_leafvs_split_gen = conn->vs_leafvs_rebuild_gen;

    if (conn->vs_compact_tid_set) {
        __wt_cond_signal(session, conn->vs_compact_cond);
        WT_TRET(__wt_thread_join(session, &conn->vs_compact_tid));
        conn->vs_compact_tid_set = false;
    }

    if (conn->vs_compact_cond != NULL)
        __wt_cond_destroy(session, &conn->vs_compact_cond);

    return (ret);
}

/*
 * __wt_vs_server_create --
 *     Initialize version store metadata and start the VS compaction server thread.
 */
int
__wt_vs_server_create(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    uint32_t session_flags;

    conn = S2C(session);

    if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED) ||
      !FLD_ISSET(conn->log_flags, WT_CONN_LOG_VERSION_STORE))
        return (0);

    WT_RET(__wt_vs_range_init(session, &conn->vs_range));

    /*
     * Force one startup refresh even if we loaded persisted metadata. The on-disk snapshot is only
     * a bootstrap hint once checkpoint-path saving is removed from the hot path.
     */
    conn->vs_leafvs_split_gen = 1;
    conn->vs_leafvs_rebuild_gen = 0;
    WT_RET_NOTFOUND_OK(__wt_vs_range_load(session, conn->vs_range));

    if (conn->vs_compact_threshold > 0) {
        session_flags = WT_SESSION_NO_DATA_HANDLES;
        WT_RET(__wt_open_internal_session(
          conn, "vs-compact-server", false, session_flags, 0, &conn->vs_compact_session));
        WT_RET(__wt_vs_compact_create(conn->vs_compact_session));
        __wt_cond_signal(session, conn->vs_compact_cond);
    }

    return (0);
}

/*
 * __wt_vs_server_destroy --
 *     Shut down the VS compaction server thread and destroy associated metadata.
 */
int
__wt_vs_server_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    if (conn->vs_compact_tid_set)
        WT_TRET(__wt_vs_compact_destroy(session));

    if (conn->vs_compact_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->vs_compact_session));
        conn->vs_compact_session = NULL;
    }

    /*
     * Do not drain remaining PrepVS work during close. Close should hand off durable backlog
     * promptly, and the next startup/background compaction pass will refresh vs_range and resume
     * redistribution.
     */
    if (conn->vs_range != NULL) {
        WT_TRET(__wt_vs_range_destroy(session, conn->vs_range));
        conn->vs_range = NULL;
    }

    return (ret);
}
