#include <test_util.h>

/*
* This code is for testing history store and further version ID history
*/

/* 
This window is divided into two parts: the stable portion, in which the composition of the data is
fixed, further write operations are prohibited, and all updates are fully durable; and the unstable
portion, in which changes to the data are still being accumulated, write operations are ongoing,
and updates may be committed and even checkpointed but are not yet fully durable. Reading from the
unstable portion of the time window is not prohibited, but if done incautiously can lead to data
inconsistency.*/

static const char *home;

int main(int argc, char *argv[])
{
    WT_CONNECTION *conn;
    WT_CURSOR *cursor;
    WT_SESSION *session;
    char value_str[32];
    char ts_str[32];
    const char *key, *value;
    int ret;

    home = example_setup(argc, argv);

    // Creating a new session to work with history store
    error_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));

    // Sessions are for a single thread of work 
    error_check(conn->open_session(conn, NULL, NULL, &session));

    // Creates a new table "blue:test_history" with string key and value formats
    error_check(session->create(session, "blue:test_history", "key_format=S,value_format=S"));

    // Opens a cursor on the "blue:test_history" table
    error_check(session->open_cursor(session, "blue:test_history", NULL, NULL, &cursor));

    // ========================== Testing History Store ==========================
    // Set oldest timestamp to 1
    error_check(conn->set_timestamp(conn, "oldest_timestamp=1"));

    for (int i = 1; i < 10; i++) {
        error_check(session->begin_transaction(session, NULL));
         // Increase timestamp for each insert
        snprintf(ts_str, sizeof(ts_str), "commit_timestamp=%x", (unsigned int)i);
        error_check(session->timestamp_transaction(session, ts_str));

        key = "Tony";
        // Increase the value for each insert
        snprintf(value_str, sizeof(value_str), "FADU VALUE %d", i);
        value = value_str;

        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        /*
        The WT_SESSION::open_cursor overwrite configuration is true by default,
        causing WT_CURSOR::insert and WT_CURSOR::update to ignore the current state of the record,
        and these methods will succeed regardless of whether or not the record previously exists. 
        Meaning it won't return any error.
        */

        error_check(cursor->insert(cursor));    
        error_check(session->commit_transaction(session, NULL));
    }

    error_check(conn->set_timestamp(conn, "stable_timestamp=a"));

    printf("Inserting more recent values...\n\n");
    for (int i = 11; i < 20; i++) {
        error_check(session->begin_transaction(session, NULL));
        // Increase timestamp for each insert
        // Time stamp is inserted in a hexadecimal string format
        snprintf(ts_str, sizeof(ts_str), "commit_timestamp=%x", (unsigned int)i);
        error_check(session->timestamp_transaction(session, ts_str));

        key = "Tony";
        // Increase the value for each insert
        snprintf(value_str, sizeof(value_str), "FADU VALUE %d", i);
        value = value_str;

        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        error_check(cursor->insert(cursor));    
        error_check(session->commit_transaction(session, NULL));
    }

    // This should read the latest value "FADU VALUE 19"
    error_check(cursor->reset(cursor)); /* Restart the scan. */
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &key));
        error_check(cursor->get_value(cursor, &value));

        printf("Got record: %s : %s\n", key, value);
    }
    scan_end_check(ret == WT_NOTFOUND); /* Check for end-of-table. */

    // printf("Checkpointing the current state...\n\n");
    error_check(session->checkpoint(session, NULL));

    printf("Closing the session...\n\n");
    error_check(session->close(session, NULL));

    printf("Closing the connection...\n\n");
    error_check(conn->close(conn, NULL));
    printf("Session closed.\n\n");
    /* Close all handles. */

    // ========================== Reading from Latest Store ==========================

    // Up to the stable version is visible
    // Reconcile does not include records with timestamps greater than the stable timestamp

#if 0
    printf("Re-opening the connection to read from the latest store...\n\n");
    error_check(wiredtiger_open(home, NULL, "create,statistics=(all)", &conn));
    error_check(conn->open_session(conn, NULL, NULL, &session));
    error_check(session->open_cursor(session, "blue:test_history", NULL, NULL, &cursor));
    error_check(cursor->reset(cursor)); /* Restart the scan. */
    while ((ret = cursor->next(cursor)) == 0) {
        error_check(cursor->get_key(cursor, &key));
        error_check(cursor->get_value(cursor, &value));

        printf("Got record: %s : %s\n", key, value);
    }
    scan_end_check(ret == WT_NOTFOUND); /* Check for end-of-table. */
    error_check(session->close(session, NULL));
    error_check(conn->close(conn, NULL));

#endif
    return (EXIT_SUCCESS);
}
