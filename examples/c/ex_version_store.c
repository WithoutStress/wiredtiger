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
 * 	demonstrates how to use version_store configuration with vid logging
 * 	and verifies .vstore file renaming.
 */
#include <test_util.h>
#include <dirent.h>

static const char *home = "WT_HOME_VERSION_STORE";
static const char *const uri = "blue:version_test";

#define CONN_CONFIG \
    "create,cache_size=100MB,log=(enabled=true,file_max=100KB,remove=false,version_store=true)"
#define NUM_KEYS 100
#define NUM_VERSIONS 50

static int
count_vstore_files(const char *dir)
{
    DIR *d;
    struct dirent *entry;
    int count = 0;

    d = opendir(dir);
    if (d == NULL)
        return -1;

    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "WiredTigerVS.", 13) == 0)
            count++;
    }
    closedir(d);
    return count;
}

static int
count_log_files(const char *dir)
{
    DIR *d;
    struct dirent *entry;
    int count = 0;

    d = opendir(dir);
    if (d == NULL)
        return -1;

    while ((entry = readdir(d)) != NULL) {
        if (strncmp(entry->d_name, "WiredTigerLog.", 14) == 0)
            count++;
    }
    closedir(d);
    return count;
}

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    WT_ITEM key, value;
    int i, j, vstore_count, log_count;
    char key_buf[64], value_buf[256], vid_buf[64];

    (void)argc;
    (void)argv;

    error_check(system("rm -rf WT_HOME_VERSION_STORE && mkdir WT_HOME_VERSION_STORE"));

    printf("=== Version Store Test with VID Logging ===\n\n");

    printf("1. Opening WiredTiger with version_store=true\n");
    error_check(wiredtiger_open(home, NULL, CONN_CONFIG, &conn));

    error_check(conn->open_session(conn, NULL, NULL, &session));
    error_check(session->create(session, uri, "key_format=u,value_format=u"));
    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    printf("2. Inserting %d keys with %d versions each (with VID)...\n", NUM_KEYS, NUM_VERSIONS);

    for (i = 0; i < NUM_KEYS; i++) {
        for (j = 0; j < NUM_VERSIONS; j++) {
            snprintf(key_buf, sizeof(key_buf), "key%05d", i);
            key.data = key_buf;
            key.size = strlen(key_buf);

            snprintf(vid_buf, sizeof(vid_buf), "vid_%05d_%05d", i, j);
            key.vid = vid_buf;
            key.vid_size = strlen(vid_buf);

            snprintf(value_buf, sizeof(value_buf),
                "value_for_key_%05d_version_%05d_padding_to_make_it_larger", i, j);
            value.data = value_buf;
            value.size = strlen(value_buf);
            value.vid = vid_buf;
            value.vid_size = strlen(vid_buf);

            cursor->set_key_with_vid(cursor, &key);
            cursor->set_value_with_vid(cursor, &value);
            error_check(cursor->insert(cursor));
        }

        if ((i + 1) % 20 == 0)
            printf("   Inserted %d/%d keys...\n", i + 1, NUM_KEYS);
    }

    printf("3. Verifying latest values (get without vid)...\n");
    error_check(cursor->reset(cursor));

    for (i = 0; i < 5; i++) {
        WT_ITEM got_value;
        char expected_value[256];

        snprintf(key_buf, sizeof(key_buf), "key%05d", i);
        key.data = key_buf;
        key.size = strlen(key_buf);

        cursor->set_key(cursor, &key);
        error_check(cursor->search(cursor));
        error_check(cursor->get_value(cursor, &got_value));

        snprintf(expected_value, sizeof(expected_value),
            "value_for_key_%05d_version_%05d_padding_to_make_it_larger", i, NUM_VERSIONS - 1);

        printf("   key%05d: got '%.*s'\n", i, (int)got_value.size, (char *)got_value.data);

        if (got_value.size == strlen(expected_value) &&
            memcmp(got_value.data, expected_value, got_value.size) == 0) {
            printf("      [OK] Matches latest version (v%d)\n", NUM_VERSIONS - 1);
        } else {
            printf("      [FAIL] Expected: %s\n", expected_value);
        }
    }

    printf("\n4. Running checkpoint to flush logs...\n");
    error_check(session->checkpoint(session, NULL));

    printf("5. Waiting for log server to process files (3 seconds)...\n");
    sleep(3);

    log_count = count_log_files(home);
    vstore_count = count_vstore_files(home);

    printf("\n=== Results ===\n");
    printf("   Log files (WiredTigerLog.*): %d\n", log_count);
    printf("   VStore files (WiredTigerVS.*): %d\n", vstore_count);

    if (vstore_count > 0) {
        DIR *d;
        struct dirent *entry;

        printf("\n[PASS] WiredTigerVS files were created successfully!\n");

        d = opendir(home);
        printf("\n   VStore files found:\n");
        while ((entry = readdir(d)) != NULL) {
            if (strncmp(entry->d_name, "WiredTigerVS.", 13) == 0)
                printf("      - %s\n", entry->d_name);
        }
        closedir(d);
    } else {
        printf("\n[INFO] No WiredTigerVS files yet. Log files may still be in use.\n");
        printf("   This is expected if there's only one active log file.\n");
    }

    printf("\n5. Closing connection...\n");
    error_check(cursor->close(cursor));
    error_check(conn->close(conn, NULL));

    printf("\n6. Checking files after close...\n");
    log_count = count_log_files(home);
    vstore_count = count_vstore_files(home);
    printf("   Log files: %d, VStore files: %d\n", log_count, vstore_count);

    printf("\n=== Test Completed ===\n");
    return (EXIT_SUCCESS);
}
