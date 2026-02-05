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
 * - Per-leaf VS files (SSTable-like structure)
 * - Compaction of WiredTigerVS log files into per-leaf VS files
 */

/* VS key range file name */
#define WT_VS_RANGE_FILE "WiredTiger.vs_range"

/* Per-leaf VS file prefix */
#define WT_VS_LEAF_PREFIX "LeafVS_"
#define WT_VS_LEAF_SUFFIX ".vs"

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
 * WT_VS_ENTRY --
 *     A single entry extracted from WiredTigerVS log file.
 */
struct __wt_vs_entry {
    uint32_t btree_id;               /* B-tree identifier */
    WT_ITEM key;                     /* Key */
    WT_ITEM vid;                     /* Version ID */
    WT_ITEM value;                   /* Value */
};

