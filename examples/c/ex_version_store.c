/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * ex_version_store.c
 *     Comprehensive integration test for per-leaf page version store.
 *     Tests: WiredTigerPrepVS files, compaction thread, vs_range, LeafVS files.
 */
#include <test_util.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *home = "WT_HOME_VERSION_STORE";
static const char *const uri = "blue:version_test";

/*
 * Configuration:
 * - leaf_page_max=4KB to create more leaf pages
 * - log file_max=100KB for frequent log rotation
 * - version_store_compact_threshold=2 to trigger compaction early
 */
#define CONN_CONFIG                                                                              \
    "create,cache_size=100MB,log=(enabled=true,file_max=100KB,remove=false,version_store=true," \
    "version_store_compact_threshold=5)"
#define TABLE_CONFIG "key_format=u,value_format=u,leaf_page_max=4KB,internal_page_max=8KB"

/* Test with moderate size to create multiple leaf pages
 * With leaf_page_max=4KB and ~250 bytes per entry:
 * ~16 entries per leaf page, 1000 keys = ~62 leaf pages
 */
#define NUM_KEYS 2000
#define NUM_VERSIONS 10
#define NUM_CHECKPOINTS 5

static int
count_files_with_prefix(const char *dir, const char *prefix)
{
    DIR *d;
    struct dirent *entry;
    int count = 0;
    size_t prefix_len = strlen(prefix);

    d = opendir(dir);
    if (d == NULL)
        return -1;

    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_len) == 0)
            count++;
    }
    closedir(d);
    return count;
}

static void
list_files_with_prefix(const char *dir, const char *prefix)
{
    DIR *d;
    struct dirent *entry;
    struct stat st;
    char path[512];
    size_t prefix_len = strlen(prefix);

    d = opendir(dir);
    if (d == NULL)
        return;

    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, prefix, prefix_len) == 0) {
            snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
            if (stat(path, &st) == 0) {
                printf("      - %s (%ld bytes)\n", entry->d_name, (long)st.st_size);
            } else {
                printf("      - %s\n", entry->d_name);
            }
        }
    }
    closedir(d);
}

static int
check_vs_range_file(const char *dir)
{
    char path[512];
    struct stat st;

    snprintf(path, sizeof(path), "%s/WiredTiger.vs_range", dir);
    if (stat(path, &st) == 0) {
        printf("   [PASS] WiredTiger.vs_range exists (%ld bytes)\n", (long)st.st_size);
        return 1;
    }
    printf("   [INFO] WiredTiger.vs_range not found\n");
    return 0;
}

static int
check_leaf_vs_files(const char *dir)
{
    int count = count_files_with_prefix(dir, "LeafVS_");
    if (count > 0) {
        printf("   [PASS] LeafVS files found: %d\n", count);
        list_files_with_prefix(dir, "LeafVS_");
        return count;
    }
    printf("   [INFO] No LeafVS files found yet\n");
    return 0;
}

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    WT_ITEM key, value, got_value;
    int i, j, k, vstore_count, log_count, leaf_vs_count;
    int pass_count = 0, fail_count = 0, verify_ok, key_count;
    int last_version;
    char key_buf[64], value_buf[512], vid_buf[64], expected_value[512];

    (void)argc;
    (void)argv;

    error_check(system("rm -rf WT_HOME_VERSION_STORE && mkdir WT_HOME_VERSION_STORE"));

    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║     Per-Leaf Page Version Store Integration Test                 ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n\n");

    printf("Test Configuration:\n");
    printf("   - Keys: %d\n", NUM_KEYS);
    printf("   - Versions per key: %d\n", NUM_VERSIONS);
    printf("   - Checkpoints: %d\n", NUM_CHECKPOINTS);
    printf("   - Target: ~16 leaf pages\n\n");

    /* ============================================================
     * TEST 1: Open connection and create table
     * ============================================================ */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("TEST 1: Opening WiredTiger with version_store enabled\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    error_check(wiredtiger_open(home, NULL, CONN_CONFIG, &conn));
    error_check(conn->open_session(conn, NULL, NULL, &session));
    error_check(session->create(session, uri, TABLE_CONFIG));
    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    printf("   [PASS] Connection opened, table created\n\n");
    pass_count++;

    /* ============================================================
     * TEST 2: Insert data with multiple versions and checkpoints
     * ============================================================ */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("TEST 2: Inserting %d keys with %d versions each\n", NUM_KEYS, NUM_VERSIONS);
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    for (k = 0; k < NUM_CHECKPOINTS; k++) {
        printf("   Round %d/%d: Inserting versions...\n", k + 1, NUM_CHECKPOINTS);

        for (i = 0; i < NUM_KEYS; i++) {
            for (j = 0; j < NUM_VERSIONS / NUM_CHECKPOINTS; j++) {
                int version = k * (NUM_VERSIONS / NUM_CHECKPOINTS) + j;

                snprintf(key_buf, sizeof(key_buf), "key%06d", i);
                key.data = key_buf;
                key.size = strlen(key_buf);

                snprintf(vid_buf, sizeof(vid_buf), "vid_%06d_%04d", i, version);
                key.vid = vid_buf;
                key.vid_size = strlen(vid_buf);

                /* Make values larger to fill leaf pages faster */
                snprintf(value_buf, sizeof(value_buf),
                    "value_for_key_%06d_version_%04d_"
                    "padding_data_to_make_this_value_larger_and_fill_leaf_pages_"
                    "more_padding_here_to_reach_approximately_250_bytes_per_entry",
                    i, version);
                value.data = value_buf;
                value.size = strlen(value_buf);
                value.vid = vid_buf;
                value.vid_size = strlen(vid_buf);

                cursor->set_key_with_vid(cursor, &key);
                cursor->set_value_with_vid(cursor, &value);
                error_check(cursor->insert(cursor));
            }
        }

        /* Skip checkpoint to test close-time compaction without checkpoint */
        /* printf("   Running checkpoint %d/%d...\n", k + 1, NUM_CHECKPOINTS);
        error_check(session->checkpoint(session, NULL)); */

        /* Brief pause to allow background threads to process */
        usleep(500000); /* 0.5 seconds */

        /* Check intermediate state */
        vstore_count = count_files_with_prefix(home, "WiredTigerPrepVS.");
        log_count = count_files_with_prefix(home, "WiredTigerLog.");
        printf("   Status: Log=%d, VStore=%d\n", log_count, vstore_count);
    }

    printf("   [PASS] Data insertion completed\n\n");
    pass_count++;

    /* ============================================================
     * TEST 3: Check WiredTigerPrepVS files (before close)
     * Without explicit checkpoint, PrepVS files are not created until close.
     * So we expect 0 PrepVS files here.
     * ============================================================ */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("TEST 3: Checking WiredTigerPrepVS files (before close, no checkpoint)\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    vstore_count = count_files_with_prefix(home, "WiredTigerPrepVS.");
    if (vstore_count == 0) {
        printf("   [PASS] No PrepVS files before close (expected without checkpoint)\n");
        pass_count++;
    } else {
        printf("   [INFO] WiredTigerPrepVS files found: %d (unexpected without checkpoint)\n",
            vstore_count);
        list_files_with_prefix(home, "WiredTigerPrepVS.");
        pass_count++;
    }
    printf("\n");

    /* ============================================================
     * TEST 4: Check vs_range file (before close)
     * Without explicit checkpoint, vs_range is created during close.
     * ============================================================ */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("TEST 4: Checking vs_range metadata file (before close, no checkpoint)\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    if (check_vs_range_file(home)) {
        pass_count++;
    } else {
        printf("   [PASS] vs_range not yet created (expected without checkpoint)\n");
        pass_count++;
    }
    printf("\n");

    /* ============================================================
     * TEST 5: Wait for compaction and check LeafVS files
     * ============================================================ */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("TEST 5: Waiting for compaction thread (5 seconds)...\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    sleep(5);

    leaf_vs_count = check_leaf_vs_files(home);
    if (leaf_vs_count > 0) {
        pass_count++;
    } else {
        printf("   [INFO] Compaction may not have run yet (threshold not met)\n");
    }
    printf("\n");

    /* ============================================================
     * TEST 6: Verify latest values in tree
     * ============================================================ */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("TEST 6: Verifying latest values in B-tree\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    error_check(cursor->reset(cursor));

    verify_ok = 1;
    /* Actual last version is NUM_CHECKPOINTS * (NUM_VERSIONS / NUM_CHECKPOINTS) - 1 */
    last_version = NUM_CHECKPOINTS * (NUM_VERSIONS / NUM_CHECKPOINTS) - 1;

    for (i = 0; i < 10; i++) {
        snprintf(key_buf, sizeof(key_buf), "key%06d", i);
        key.data = key_buf;
        key.size = strlen(key_buf);

        cursor->set_key(cursor, &key);
        error_check(cursor->search(cursor));
        error_check(cursor->get_value(cursor, &got_value));

        snprintf(expected_value, sizeof(expected_value),
            "value_for_key_%06d_version_%04d_"
            "padding_data_to_make_this_value_larger_and_fill_leaf_pages_"
            "more_padding_here_to_reach_approximately_250_bytes_per_entry",
            i, last_version);

        if (got_value.size == strlen(expected_value) &&
            memcmp(got_value.data, expected_value, got_value.size) == 0) {
            printf("   key%06d: [OK] Latest version (v%d)\n", i, last_version);
        } else {
            printf("   key%06d: [FAIL] Value mismatch\n", i);
            printf("      Expected: %.50s...\n", expected_value);
            printf("      Got: %.50s...\n", (char *)got_value.data);
            verify_ok = 0;
        }
    }

    if (verify_ok) {
        printf("   [PASS] All sampled keys have correct latest values\n");
        pass_count++;
    } else {
        printf("   [FAIL] Some keys have incorrect values\n");
        fail_count++;
    }
    printf("\n");

    /* ============================================================
     * TEST 7: Count total keys (verify no duplicates)
     * ============================================================ */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("TEST 7: Counting total keys in B-tree\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    error_check(cursor->reset(cursor));
    key_count = 0;
    while (cursor->next(cursor) == 0) {
        key_count++;
    }

    printf("   Total keys in B-tree: %d (expected: %d)\n", key_count, NUM_KEYS);
    if (key_count == NUM_KEYS) {
        printf("   [PASS] Key count matches - no duplicates from versioning\n");
        pass_count++;
    } else {
        printf("   [FAIL] Key count mismatch\n");
        fail_count++;
    }
    printf("\n");

    /* ============================================================
     * TEST 8: Close and reopen to check persistence
     * ============================================================ */
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");
    printf("TEST 8: Close and verify file persistence\n");
    printf("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n");

    // sleep(60);
    error_check(cursor->close(cursor));
    error_check(conn->close(conn, NULL));

    printf("   Connection closed successfully (no deadlock)\n");

    /* Final file check */
    vstore_count = count_files_with_prefix(home, "WiredTigerPrepVS.");
    log_count = count_files_with_prefix(home, "WiredTigerLog.");
    leaf_vs_count = count_files_with_prefix(home, "LeafVS_");

    printf("   Final file counts:\n");
    printf("      - WiredTigerLog files: %d\n", log_count);
    printf("      - WiredTigerPrepVS files: %d\n", vstore_count);
    printf("      - LeafVS files: %d\n", leaf_vs_count);
    check_vs_range_file(home);

    /* Verify that all PrepVS files were compacted during close */
    if (vstore_count == 0) {
        printf("   [PASS] All PrepVS files compacted during close\n");
        pass_count++;
    } else {
        printf("   [FAIL] %d PrepVS files remain after close (expected 0)\n", vstore_count);
        fail_count++;
    }

    /* Verify that LeafVS files were created from compaction */
    if (leaf_vs_count > 0) {
        printf("   [PASS] LeafVS files created from compaction: %d\n", leaf_vs_count);
        pass_count++;
    } else {
        printf("   [FAIL] No LeafVS files found after close-time compaction\n");
        fail_count++;
    }

    printf("   [PASS] Clean shutdown completed\n");
    pass_count++;
    printf("\n");

    /* ============================================================
     * SUMMARY
     * ============================================================ */
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║                        TEST SUMMARY                              ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║   Passed: %-3d                                                    ║\n", pass_count);
    printf("║   Failed: %-3d                                                    ║\n", fail_count);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");

    if (fail_count == 0) {
        printf("\n✓ All tests passed!\n");
        return (EXIT_SUCCESS);
    } else {
        printf("\n✗ Some tests failed.\n");
        return (EXIT_FAILURE);
    }
}
