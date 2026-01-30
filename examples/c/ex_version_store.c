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
 * 	demonstrates how to use version_store configuration with logging.
 */
#include <test_util.h>

static const char *home = "WT_HOME_VERSION_STORE";
static const char *const uri = "table:version_test";

#define CONN_CONFIG_VERSION_STORE "create,cache_size=100MB,log=(enabled=true,remove=false,version_store=true)"
#define CONN_CONFIG_NO_VERSION_STORE "create,cache_size=100MB,log=(enabled=true,remove=false,version_store=false)"
#define MAX_KEYS 10

int
main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    int i;
    char key_buf[64], value_buf[64];
    bool use_version_store;

    use_version_store = (argc > 1 && strcmp(argv[1], "true") == 0);

    error_check(system("rm -rf WT_HOME_VERSION_STORE && mkdir WT_HOME_VERSION_STORE"));

    printf("Opening WiredTiger with version_store=%s\n", use_version_store ? "true" : "false");
    
    error_check(wiredtiger_open(home, NULL, 
        use_version_store ? CONN_CONFIG_VERSION_STORE : CONN_CONFIG_NO_VERSION_STORE, 
        &conn));

    error_check(conn->open_session(conn, NULL, NULL, &session));
    error_check(session->create(session, uri, "key_format=S,value_format=S"));

    error_check(session->open_cursor(session, uri, NULL, NULL, &cursor));

    error_check(session->begin_transaction(session, NULL));
    
    for (i = 0; i < MAX_KEYS; i++) {
        snprintf(key_buf, sizeof(key_buf), "key%d", i);
        snprintf(value_buf, sizeof(value_buf), "value%d", i);
        cursor->set_key(cursor, key_buf);
        cursor->set_value(cursor, value_buf);
        error_check(cursor->insert(cursor));
    }
    
    error_check(session->commit_transaction(session, NULL));

    printf("Inserted %d records\n", MAX_KEYS);

    error_check(cursor->close(cursor));
    error_check(conn->close(conn, NULL));

    printf("Test completed successfully!\n");
    return (EXIT_SUCCESS);
}
