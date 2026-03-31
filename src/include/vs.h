/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * Version Store (VS) subsystem for per-leaf page version storage.
 *
 * This module manages:
 * - Key range metadata for mapping keys to leaf pages
 * - Per-leaf LeafVS files (SSTable-like structure)
 * - Compaction of WiredTigerPrepVS log files into per-leaf LeafVS files
 *
 * Terminology:
 * - PrepVS (Preparatory VS): Log-structured temp files (WiredTigerPrepVS.*)
 * - LeafVS: Per-leaf page version files (LeafVS_<btree_id>_<page_id>.vs)
 */

/* VS key range file name */
#define WT_VS_RANGE_FILE "WiredTiger.vs_range"

/* PrepVS (preparatory VS) file prefix - log-structured temp files */
#define WT_PREPVS_PREFIX "WiredTigerPrepVS."

/* LeafVS (per-leaf VS) file prefix and suffix */
#define WT_LEAFVS_PREFIX "LeafVS_"
#define WT_LEAFVS_SUFFIX ".vs"

/* VS file magic number */
#define WT_VS_RANGE_MAGIC "WTVS"
#define WT_VS_RANGE_VERSION 1

/*
 * WT_VS_KEY_RANGE --
 *     Key range for a single leaf page.
 */
struct __wt_vs_key_range {
    uint64_t page_id;        /* Leaf page identifier (address) */
    WT_ITEM min_key;         /* First key in this leaf */
    WT_ITEM max_key;         /* Last key in this leaf */
};

/*
 * WT_VS_BTREE_RANGE --
 *     Key range metadata for a single B-tree.
 */
struct __wt_vs_btree_range {
    uint32_t btree_id;                /* B-tree identifier */
    char *uri;                        /* B-tree URI */
    WT_VS_KEY_RANGE *ranges;          /* Array of key ranges (sorted by min_key) */
    uint32_t range_count;             /* Number of key ranges */
    uint32_t range_alloc;             /* Allocated size of ranges array */
};

/*
 * WT_VS_RANGE --
 *     Global version store key range metadata.
 */
struct __wt_vs_range {
    WT_VS_BTREE_RANGE *btrees;         /* Array of B-tree metadata */
    uint32_t btree_count;             /* Number of B-trees */
    uint32_t btree_alloc;             /* Allocated size of btrees array */
    uint64_t checkpoint_gen;          /* Checkpoint generation when recorded */
    
    WT_RWLOCK lock;                   /* Protects metadata access */
};

/*
 * WT_PREPVS_ENTRY --
 *     A single entry extracted from WiredTigerPrepVS log file.
 */
struct __wt_prepvs_entry {
    uint32_t btree_id;               /* B-tree identifier */
    WT_ITEM key;                     /* Key */
    WT_ITEM vid;                     /* Version ID */
    WT_ITEM value;                   /* Value */
};

typedef int (*WT_PREPVS_CHUNK_HANDLER)(
  WT_SESSION_IMPL *session, WT_PREPVS_ENTRY *entries, uint32_t count, void *cookie);

/*
 * Per-leaf VS file format structures.
 * File name: LeafVS_<btree_id>_<page_id>.vs
 */

/* LeafVS file magic */
#define WT_LEAFVS_MAGIC "LVSS"
#define WT_LEAFVS_FORMAT_VERSION 1

/*
 * WT_LEAFVS_FILE_HEADER --
 *     Header for per-leaf LeafVS file.
 */
struct __wt_leafvs_file_header {
    char magic[4];                   /* "LVSS" */
    uint32_t version;                /* Format version */
    uint32_t btree_id;               /* B-tree identifier */
    uint64_t page_id;                /* Leaf page identifier */
    uint64_t key_count;              /* Number of unique keys */
    uint64_t version_count;          /* Total number of versions */
    uint64_t index_offset;           /* Offset to index section */
    uint64_t data_size;              /* Size of data section */
};

/*
 * WT_LEAFVS_VER --
 *     A single version of a key (vid + value).
 */
struct __wt_leafvs_ver {
    WT_ITEM vid;                     /* Version ID */
    WT_ITEM value;                   /* Value for this version */
};

/*
 * WT_LEAFVS_KEY_GROUP --
 *     All versions of a single key.
 */
struct __wt_leafvs_key_group {
    WT_ITEM key;                     /* The key */
    WT_LEAFVS_VER *versions;         /* Array of versions (sorted by vid) */
    uint32_t version_count;          /* Number of versions */
    uint32_t version_alloc;          /* Allocated size */
};

/*
 * WT_LEAFVS_FILE --
 *     In-memory representation of a per-leaf LeafVS file.
 */
struct __wt_leafvs_file {
    uint32_t btree_id;               /* B-tree identifier */
    uint64_t page_id;                /* Leaf page identifier */
    WT_LEAFVS_KEY_GROUP *groups;     /* Array of key groups (sorted by key) */
    uint32_t group_count;            /* Number of key groups */
    uint32_t group_alloc;            /* Allocated size */
};
