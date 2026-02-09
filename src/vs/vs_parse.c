/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Structure to hold parsed entries from a PrepVS log file.
 */
typedef struct {
    WT_PREPVS_ENTRY *entries;
    uint32_t count;
    uint32_t alloc;
} WT_PREPVS_PARSE_STATE;

/*
 * __prepvs_parse_record --
 *     Callback for log scan to parse a single record.
 */
static int
__prepvs_parse_record(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp,
    WT_LSN *next_lsnp, void *cookie, int firstrecord)
{
    WT_DECL_RET;
    WT_PREPVS_PARSE_STATE *state;
    WT_PREPVS_ENTRY *entry;
    const uint8_t *p, *end;
    uint32_t fileid, opsize, optype, rectype;

    WT_UNUSED(lsnp);
    WT_UNUSED(next_lsnp);
    WT_UNUSED(firstrecord);

    state = (WT_PREPVS_PARSE_STATE *)cookie;
    p = WT_LOG_SKIP_HEADER(record->data);
    end = (const uint8_t *)record->data + record->size;

    /* Read record type */
    WT_RET(__wt_logrec_read(session, &p, end, &rectype));

    /* Only process commit records */
    if (rectype != WT_LOGREC_COMMIT)
        return (0);

    /* Skip txnid */
    {
        uint64_t txnid;
        WT_RET(__wt_vunpack_uint(&p, WT_PTRDIFF(end, p), &txnid));
    }

    /* Process operations in this record */
    while (p < end) {
        /* Need at least 8 bytes for optype + opsize */
        if ((size_t)(end - p) < 8)
            break;

        ret = __wt_logop_read(session, &p, end, &optype, &opsize);
        if (ret != 0 || opsize == 0 || opsize > (uint32_t)(end - p))
            break;

        /* Only process row put with VID operations */
        if (optype == WT_LOGOP_ROW_PUT_VID) {
            WT_ITEM key, value;

            /* Allocate more space if needed */
            if (state->count >= state->alloc) {
                uint32_t new_alloc = state->count + 16;
                WT_PREPVS_ENTRY *new_entries;

                WT_RET(__wt_calloc_def(session, new_alloc, &new_entries));
                if (state->entries != NULL) {
                    memcpy(new_entries, state->entries, state->count * sizeof(WT_PREPVS_ENTRY));
                    __wt_free(session, state->entries);
                }
                state->entries = new_entries;
                state->alloc = new_alloc;
            }

            entry = &state->entries[state->count];
            memset(entry, 0, sizeof(WT_PREPVS_ENTRY));

            /* Unpack the row put with VID */
            WT_RET(__wt_logop_row_put_unpack_with_vid(session, &p, end,
                &fileid, &key, &value));

            entry->btree_id = fileid;

            /* Copy key */
            WT_RET(__wt_buf_set(session, &entry->key, key.data, key.size));

            /* Copy value */
            if (value.size > 0)
                WT_RET(__wt_buf_set(session, &entry->value, value.data, value.size));

            /* Copy vid */
            if (key.vid_size > 0)
                WT_RET(__wt_buf_set(session, &entry->vid, key.vid, key.vid_size));

            state->count++;
        } else {
            /* Skip other operation types */
            p += opsize;
        }
    }

    return (0);
}

/*
 * __wt_prepvs_parse_file --
 *     Parse a WiredTigerPrepVS file and extract all entries.
 */
int
__wt_prepvs_parse_file(WT_SESSION_IMPL *session, const char *filename,
    WT_PREPVS_ENTRY **entriesp, uint32_t *countp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_FH *fh;
    WT_ITEM buf;
    WT_LOG_RECORD *logrec;
    WT_LSN lsn;
    WT_PREPVS_PARSE_STATE state;
    size_t allocsize, offset;
    wt_off_t file_size;
    uint32_t lognum;
    bool need_salvage;

    conn = S2C(session);
    fh = NULL;
    memset(&buf, 0, sizeof(buf));
    memset(&state, 0, sizeof(state));
    allocsize = (conn->log != NULL) ? conn->log->allocsize : WT_LOG_ALIGN;

    /* Extract log number from filename (WiredTigerPrepVS.NNNNNNNNNN) */
    {
        const char *basename;
        basename = strrchr(filename, '/');
        basename = basename != NULL ? basename + 1 : filename;
        if (sscanf(basename, WT_PREPVS_PREFIX "%010" SCNu32, &lognum) != 1)
            WT_RET_MSG(session, EINVAL, "Invalid PrepVS filename: %s", filename);
    }

    /* Open the VS file */
    WT_RET(__wt_open(session, filename, WT_FS_OPEN_FILE_TYPE_REGULAR,
        WT_FS_OPEN_READONLY, &fh));

    /* Get file size */
    WT_ERR(__wt_filesize(session, fh, &file_size));

    /* Skip the log file header */
    offset = allocsize;

    /* Read and process records */
    while ((wt_off_t)offset < file_size) {
        WT_ITEM record;
        uint32_t reclen;

        /* Read the record header */
        WT_ERR(__wt_buf_grow(session, &buf, sizeof(WT_LOG_RECORD)));
        WT_ERR(__wt_read(session, fh, (wt_off_t)offset, sizeof(WT_LOG_RECORD), buf.mem));
        logrec = (WT_LOG_RECORD *)buf.mem;
        __wt_log_record_byteswap(logrec);

        reclen = logrec->len;
        if (reclen == 0 || reclen > (uint32_t)(file_size - (wt_off_t)offset))
            break;

        /* Read the full record */
        WT_ERR(__wt_buf_grow(session, &buf, reclen));
        WT_ERR(__wt_read(session, fh, (wt_off_t)offset, reclen, buf.mem));
        logrec = (WT_LOG_RECORD *)buf.mem;
        __wt_log_record_byteswap(logrec);

        /* Verify checksum */
        need_salvage = false;
        {
            uint32_t saved_checksum, computed_checksum;
            saved_checksum = logrec->checksum;
            logrec->checksum = 0;
            computed_checksum = __wt_checksum(logrec, reclen);
            logrec->checksum = saved_checksum;
            if (saved_checksum != computed_checksum)
                need_salvage = true;
        }

        if (!need_salvage) {
            /* Process the record */
            WT_SET_LSN(&lsn, lognum, (uint32_t)offset);
            record.data = buf.mem;
            record.size = reclen;
            ret = __prepvs_parse_record(session, &record, &lsn, NULL, &state, 0);
            if (ret != 0)
                goto err;
        }

        /* Move to next record (aligned) */
        offset += WT_ALIGN(reclen, allocsize);
    }

    *entriesp = state.entries;
    *countp = state.count;
    state.entries = NULL;

err:
    if (state.entries != NULL) {
        uint32_t i;
        for (i = 0; i < state.count; i++) {
            __wt_buf_free(session, &state.entries[i].key);
            __wt_buf_free(session, &state.entries[i].vid);
            __wt_buf_free(session, &state.entries[i].value);
        }
        __wt_free(session, state.entries);
    }
    __wt_buf_free(session, &buf);
    if (fh != NULL)
        WT_TRET(__wt_close(session, &fh));
    return (ret);
}

/*
 * __wt_prepvs_entries_free --
 *     Free an array of PrepVS entries.
 */
void
__wt_prepvs_entries_free(WT_SESSION_IMPL *session, WT_PREPVS_ENTRY **entriesp, uint32_t count)
{
    WT_PREPVS_ENTRY *entries;
    uint32_t i;

    entries = *entriesp;
    if (entries == NULL)
        return;

    for (i = 0; i < count; i++) {
        __wt_buf_free(session, &entries[i].key);
        __wt_buf_free(session, &entries[i].vid);
        __wt_buf_free(session, &entries[i].value);
    }
    __wt_free(session, entries);
    *entriesp = NULL;
}
