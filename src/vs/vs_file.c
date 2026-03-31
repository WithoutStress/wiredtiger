/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __leafleafvs_file_path --
 *     Build the path for a per-leaf VS file.
 */
static int
__leafleafvs_file_path(WT_SESSION_IMPL *session, uint32_t btree_id, uint64_t page_id,
    WT_ITEM *path)
{
    WT_RET(__wt_buf_fmt(session, path, "%s%u_%llu%s",
        WT_LEAFVS_PREFIX, btree_id, (unsigned long long)page_id, WT_LEAFVS_SUFFIX));
    return (0);
}

/*
 * __wt_leafvs_file_init --
 *     Initialize a VS file structure.
 */
int
__wt_leafvs_file_init(WT_SESSION_IMPL *session, WT_LEAFVS_FILE **leafvs_filep,
    uint32_t btree_id, uint64_t page_id)
{
    WT_LEAFVS_FILE *leafvs_file;

    WT_RET(__wt_calloc_one(session, &leafvs_file));
    leafvs_file->btree_id = btree_id;
    leafvs_file->page_id = page_id;
    leafvs_file->groups = NULL;
    leafvs_file->group_count = 0;
    leafvs_file->group_alloc = 0;

    *leafvs_filep = leafvs_file;
    return (0);
}

/*
 * __wt_leafvs_file_destroy --
 *     Destroy a VS file structure.
 */
int
__wt_leafvs_file_destroy(WT_SESSION_IMPL *session, WT_LEAFVS_FILE **leafvs_filep)
{
    WT_LEAFVS_FILE *leafvs_file;
    WT_LEAFVS_KEY_GROUP *group;
    WT_LEAFVS_VER *ver;
    uint32_t i, j;

    leafvs_file = *leafvs_filep;
    if (leafvs_file == NULL)
        return (0);

    /* Free all key groups and their versions */
    for (i = 0; i < leafvs_file->group_count; i++) {
        group = &leafvs_file->groups[i];
        
        /* Free versions */
        for (j = 0; j < group->version_count; j++) {
            ver = &group->versions[j];
            __wt_buf_free(session, &ver->vid);
            __wt_buf_free(session, &ver->value);
        }
        __wt_free(session, group->versions);
        
        /* Free key */
        __wt_buf_free(session, &group->key);
    }
    __wt_free(session, leafvs_file->groups);
    __wt_free(session, leafvs_file);
    *leafvs_filep = NULL;
    return (0);
}

/*
 * __leafleafvs_file_find_group --
 *     Find or create a key group for the given key.
 */
static int
__leafleafvs_file_find_group(WT_SESSION_IMPL *session, WT_LEAFVS_FILE *leafvs_file,
    const WT_ITEM *key, WT_LEAFVS_KEY_GROUP **groupp)
{
    WT_LEAFVS_KEY_GROUP *group;
    uint32_t i;
    int cmp;

    /* Binary search for the key */
    for (i = 0; i < leafvs_file->group_count; i++) {
        group = &leafvs_file->groups[i];
        cmp = memcmp(key->data, group->key.data,
            WT_MIN(key->size, group->key.size));
        if (cmp == 0 && key->size == group->key.size) {
            *groupp = group;
            return (0);
        }
        if (cmp < 0 || (cmp == 0 && key->size < group->key.size))
            break;
    }

    /* Key not found, insert a new group */
    {
        size_t alloc_size = leafvs_file->group_alloc;
        WT_RET(__wt_realloc_def(session, &alloc_size,
            leafvs_file->group_count + 1, &leafvs_file->groups));
        leafvs_file->group_alloc = (uint32_t)alloc_size;
    }
    
    /* Shift groups to make room */
    if (i < leafvs_file->group_count)
        memmove(&leafvs_file->groups[i + 1], &leafvs_file->groups[i],
            (leafvs_file->group_count - i) * sizeof(WT_LEAFVS_KEY_GROUP));
    
    /* Initialize new group */
    group = &leafvs_file->groups[i];
    memset(group, 0, sizeof(WT_LEAFVS_KEY_GROUP));
    WT_RET(__wt_buf_set(session, &group->key, key->data, key->size));
    leafvs_file->group_count++;
    
    *groupp = group;
    return (0);
}

/*
 * __wt_leafvs_file_add_entry --
 *     Add a versioned entry to the VS file.
 */
int
__wt_leafvs_file_add_entry(WT_SESSION_IMPL *session, WT_LEAFVS_FILE *leafvs_file,
    const WT_ITEM *key, const WT_ITEM *vid, const WT_ITEM *value)
{
    WT_LEAFVS_KEY_GROUP *group;
    WT_LEAFVS_VER *ver;

    /* Find or create the key group */
    WT_RET(__leafleafvs_file_find_group(session, leafvs_file, key, &group));

    /* Add version to the group */
    {
        size_t alloc_size = group->version_alloc;
        WT_RET(__wt_realloc_def(session, &alloc_size,
            group->version_count + 1, &group->versions));
        group->version_alloc = (uint32_t)alloc_size;
    }
    
    ver = &group->versions[group->version_count];
    memset(ver, 0, sizeof(WT_LEAFVS_VER));
    WT_RET(__wt_buf_set(session, &ver->vid, vid->data, vid->size));
    WT_RET(__wt_buf_set(session, &ver->value, value->data, value->size));
    group->version_count++;

    return (0);
}

/*
 * __wt_leafvs_file_save --
 *     Save VS file to disk.
 */
int
__wt_leafvs_file_save(WT_SESSION_IMPL *session, WT_LEAFVS_FILE *leafvs_file)
{
    WT_DECL_ITEM(buf);
    WT_DECL_ITEM(path);
    WT_DECL_RET;
    WT_FH *fh;
    WT_LEAFVS_FILE_HEADER header;
    WT_LEAFVS_KEY_GROUP *group;
    WT_LEAFVS_VER *ver;
    uint32_t i, j;
    uint64_t total_versions;
    size_t offset;

    fh = NULL;
    WT_RET(__wt_scr_alloc(session, 0, &buf));
    WT_RET(__wt_scr_alloc(session, 0, &path));

    /* Build file path */
    WT_ERR(__leafleafvs_file_path(session, leafvs_file->btree_id, leafvs_file->page_id, path));

    /* Count total versions */
    total_versions = 0;
    for (i = 0; i < leafvs_file->group_count; i++)
        total_versions += leafvs_file->groups[i].version_count;

    /* Open file for writing */
    WT_ERR(__wt_open(session, path->data, WT_FS_OPEN_FILE_TYPE_REGULAR,
        WT_FS_OPEN_CREATE, &fh));

    /* Write header */
    memset(&header, 0, sizeof(header));
    memcpy(header.magic, WT_LEAFVS_MAGIC, 4);
    header.version = WT_LEAFVS_FORMAT_VERSION;
    header.btree_id = leafvs_file->btree_id;
    header.page_id = leafvs_file->page_id;
    header.key_count = leafvs_file->group_count;
    header.version_count = total_versions;

    offset = 0;
    WT_ERR(__wt_write(session, fh, (wt_off_t)offset, sizeof(header), &header));
    offset += sizeof(header);

    /* Write key groups */
    for (i = 0; i < leafvs_file->group_count; i++) {
        group = &leafvs_file->groups[i];

        /* Write: key_size(4) | version_count(4) | key_data */
        {
            uint32_t ks = (uint32_t)group->key.size;
            WT_ERR(__wt_buf_grow(session, buf, 8 + group->key.size));
            memcpy(buf->mem, &ks, 4);
            memcpy((uint8_t *)buf->mem + 4, &group->version_count, 4);
            memcpy((uint8_t *)buf->mem + 8, group->key.data, group->key.size);
        }
        buf->size = 8 + group->key.size;
        WT_ERR(__wt_write(session, fh, (wt_off_t)offset, buf->size, buf->mem));
        offset += buf->size;

        /* Write versions */
        for (j = 0; j < group->version_count; j++) {
            ver = &group->versions[j];

            /* Write: vid_size(4) | value_size(4) | vid_data | value_data */
            {
                uint32_t vs = (uint32_t)ver->vid.size;
                uint32_t vals = (uint32_t)ver->value.size;
                WT_ERR(__wt_buf_grow(session, buf, 8 + ver->vid.size + ver->value.size));
                memcpy(buf->mem, &vs, 4);
                memcpy((uint8_t *)buf->mem + 4, &vals, 4);
                memcpy((uint8_t *)buf->mem + 8, ver->vid.data, ver->vid.size);
                memcpy((uint8_t *)buf->mem + 8 + ver->vid.size, ver->value.data, ver->value.size);
            }
            buf->size = 8 + ver->vid.size + ver->value.size;
            WT_ERR(__wt_write(session, fh, (wt_off_t)offset, buf->size, buf->mem));
            offset += buf->size;
        }
    }

    /* Update header with data size */
    header.data_size = (uint64_t)offset;
    WT_ERR(__wt_write(session, fh, (wt_off_t)0, sizeof(header), &header));

err:
    if (fh != NULL)
        WT_TRET(__wt_close(session, &fh));
    __wt_scr_free(session, &buf);
    __wt_scr_free(session, &path);
    return (ret);
}

/*
 * __wt_leafvs_file_load_path --
 *     Load a LeafVS file from an explicit path.
 */
int
__wt_leafvs_file_load_path(
  WT_SESSION_IMPL *session, const char *filename, WT_LEAFVS_FILE **leafvs_filep)
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_FH *fh;
    WT_LEAFVS_FILE *leafvs_file;
    WT_LEAFVS_FILE_HEADER header;
    WT_LEAFVS_KEY_GROUP *group;
    WT_LEAFVS_VER *ver;
    uint32_t key_size, version_count, vid_size, value_size;
    uint64_t i, j;
    size_t offset;
    bool exists;

    fh = NULL;
    leafvs_file = NULL;
    WT_RET(__wt_scr_alloc(session, 0, &buf));

    /* Check if file exists */
    WT_ERR(__wt_fs_exist(session, filename, &exists));
    if (!exists) {
        ret = WT_NOTFOUND;
        goto err;
    }

    /* Open file for reading */
    WT_ERR(__wt_open(session, filename, WT_FS_OPEN_FILE_TYPE_REGULAR, 0, &fh));

    /* Read header */
    offset = 0;
    WT_ERR(__wt_read(session, fh, (wt_off_t)offset, sizeof(header), &header));
    offset += sizeof(header);

    /* Validate header */
    if (memcmp(header.magic, WT_LEAFVS_MAGIC, 4) != 0 ||
        header.version != WT_LEAFVS_FORMAT_VERSION) {
        ret = WT_ERROR;
        goto err;
    }

    /* Initialize VS file */
    WT_ERR(__wt_leafvs_file_init(session, &leafvs_file, header.btree_id, header.page_id));

    /* Read key groups */
    for (i = 0; i < header.key_count; i++) {
        /* Read key_size and version_count */
        WT_ERR(__wt_buf_grow(session, buf, 8));
        WT_ERR(__wt_read(session, fh, (wt_off_t)offset, 8, buf->mem));
        offset += 8;
        memcpy(&key_size, buf->mem, 4);
        memcpy(&version_count, (uint8_t *)buf->mem + 4, 4);

        /* Read key data */
        WT_ERR(__wt_buf_grow(session, buf, key_size));
        WT_ERR(__wt_read(session, fh, (wt_off_t)offset, key_size, buf->mem));
        offset += key_size;
        buf->size = key_size;

        /* Add key group */
        {
            size_t alloc_size = leafvs_file->group_alloc;
            WT_ERR(__wt_realloc_def(session, &alloc_size,
                leafvs_file->group_count + 1, &leafvs_file->groups));
            leafvs_file->group_alloc = (uint32_t)alloc_size;
        }
        group = &leafvs_file->groups[leafvs_file->group_count];
        memset(group, 0, sizeof(WT_LEAFVS_KEY_GROUP));
        WT_ERR(__wt_buf_set(session, &group->key, buf->mem, key_size));
        leafvs_file->group_count++;

        /* Read versions */
        for (j = 0; j < version_count; j++) {
            /* Read vid_size and value_size */
            WT_ERR(__wt_read(session, fh, (wt_off_t)offset, 8, buf->mem));
            offset += 8;
            memcpy(&vid_size, buf->mem, 4);
            memcpy(&value_size, (uint8_t *)buf->mem + 4, 4);

            /* Read vid and value */
            {
                size_t alloc_size = group->version_alloc;
                WT_ERR(__wt_realloc_def(session, &alloc_size,
                    group->version_count + 1, &group->versions));
                group->version_alloc = (uint32_t)alloc_size;
            }
            ver = &group->versions[group->version_count];
            memset(ver, 0, sizeof(WT_LEAFVS_VER));

            WT_ERR(__wt_buf_grow(session, buf, vid_size));
            WT_ERR(__wt_read(session, fh, (wt_off_t)offset, vid_size, buf->mem));
            offset += vid_size;
            WT_ERR(__wt_buf_set(session, &ver->vid, buf->mem, vid_size));

            WT_ERR(__wt_buf_grow(session, buf, value_size));
            WT_ERR(__wt_read(session, fh, (wt_off_t)offset, value_size, buf->mem));
            offset += value_size;
            WT_ERR(__wt_buf_set(session, &ver->value, buf->mem, value_size));

            group->version_count++;
        }
    }

    *leafvs_filep = leafvs_file;
    leafvs_file = NULL;

err:
    if (leafvs_file != NULL)
        WT_TRET(__wt_leafvs_file_destroy(session, &leafvs_file));
    if (fh != NULL)
        WT_TRET(__wt_close(session, &fh));
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_leafvs_file_load --
 *     Load VS file from disk.
 */
int
__wt_leafvs_file_load(
  WT_SESSION_IMPL *session, uint32_t btree_id, uint64_t page_id, WT_LEAFVS_FILE **leafvs_filep)
{
    WT_DECL_ITEM(path);
    WT_DECL_RET;

    WT_RET(__wt_scr_alloc(session, 0, &path));
    WT_ERR(__leafleafvs_file_path(session, btree_id, page_id, path));
    WT_ERR(__wt_leafvs_file_load_path(session, path->data, leafvs_filep));

err:
    __wt_scr_free(session, &path);
    return (ret);
}

/*
 * __wt_leafvs_file_search --
 *     Search for a key with specific vid in the VS file.
 */
int
__wt_leafvs_file_search(WT_SESSION_IMPL *session, WT_LEAFVS_FILE *leafvs_file,
    const WT_ITEM *key, const WT_ITEM *vid, WT_ITEM *value)
{
    WT_LEAFVS_KEY_GROUP *group;
    WT_LEAFVS_VER *ver;
    uint32_t i, j;
    int cmp;

    /* Find the key group */
    for (i = 0; i < leafvs_file->group_count; i++) {
        group = &leafvs_file->groups[i];
        cmp = memcmp(key->data, group->key.data,
            WT_MIN(key->size, group->key.size));
        if (cmp == 0 && key->size == group->key.size) {
            /* Found key, search for vid */
            for (j = 0; j < group->version_count; j++) {
                ver = &group->versions[j];
                if (vid->size == ver->vid.size &&
                    memcmp(vid->data, ver->vid.data, vid->size) == 0) {
                    /* Found version */
                    WT_RET(__wt_buf_set(session, value,
                        ver->value.data, ver->value.size));
                    return (0);
                }
            }
            return (WT_NOTFOUND);
        }
        if (cmp < 0)
            break;
    }

    return (WT_NOTFOUND);
}
