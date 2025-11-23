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
 * ex_with_vid.c
 * 	demonstrates how to use a transaction.
 */
#include <test_util.h>

static const char *home;

static void
insert_u_with_vid_example(int iteration) {
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    WT_ITEM key, value;
    int ret = 0;

    /* Open a connection to the database, creating it if necessary. */
    error_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));

    /* Open a session handle for the database. */
    error_check(conn->open_session(conn, NULL, NULL, &session));
    /*! [transaction example connection] */

    /*! [transaction example table create] */
    error_check(session->create(session, "blue:with_vid", "key_format=u,value_format=u"));
    /*! [transaction example table create] */

    /*! [transaction example cursor open] */
    error_check(session->open_cursor(session, "blue:with_vid", NULL, NULL, &cursor));
    /*! [transaction example cursor open] */

    // /*! [transaction example transaction begin] */
    // error_check(session->begin_transaction(session, "isolation=snapshot"));
    // /*! [transaction example transaction begin] */

    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < iteration; j++) {
            char key_str[16] = {0, };
            char value_str[16] = {0, };
            char version_str[16] = {0, };

            snprintf(key_str, sizeof(key_str), "KEY%d", i);
            key.data = key_str;
            key.size = strlen(key_str);

            snprintf(version_str, sizeof(version_str), "v%d", j);
            key.vid = version_str;
            key.vid_size = strlen(version_str);

            snprintf(value_str, sizeof(value_str), "VALUE%d", j);
            value.data = value_str;
            value.size = strlen(value_str);
        
            value.vid = version_str;
            value.vid_size = strlen(version_str);

            cursor->set_key_with_vid(cursor, &key);
            cursor->set_value_with_vid(cursor, &value);
            error_check(cursor->insert(cursor));
        }
    }


    // When we remove this..
    // Even after we remove this key, we should be able to access it through Version Store.
    // cursor->set_key(cursor, &key);
    // error_check(cursor->remove(cursor));

    // /*! [transaction example transaction commit] */
    // error_check(session->commit_transaction(session, NULL));
    // /*! [transaction example transaction commit] */

    printf("starting checkpoint\n");

    error_check(session->checkpoint(session, NULL));

    printf("done with checkpoint\n");

    /*! [transaction example cursor list] */
    error_check(cursor->reset(cursor)); /* Restart the scan. */
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &key));
        error_check(cursor->get_value(cursor, &value));

        printf("Got record: %s : %s\n", (const char*)key.data, (const char*)value.data);
    }
    scan_end_check(ret == WT_NOTFOUND); /* Check for end-of-table. */
    /*! [transaction example cursor list] */

    /*! [transaction example close] */
    error_check(conn->close(conn, NULL)); /* Close all handles. */
                                          /*! [transaction example close] */
}

int
main(int argc, char *argv[])
{
    int iteration;
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <number of iterations>\n", argv[0]);
        return (EXIT_FAILURE);
    }

    iteration = atoi(argv[1]);
    home = example_setup(argc, argv);

    insert_u_with_vid_example(iteration);

    return (EXIT_SUCCESS);
}
