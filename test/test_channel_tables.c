#include "db.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB "test_channel_tables.db"

static void test_channels_table(void) {
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    /* Insert a channel */
    int rc = sqlite3_exec(db,
        "INSERT INTO channels(name, type, binary_path) VALUES('telegram','telegram','/usr/local/bin/channel_telegram')",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
    /* Read it back */
    sqlite3_stmt *st;
    rc = sqlite3_prepare_v2(db, "SELECT name, type, binary_path, status FROM channels WHERE name='telegram'", -1, &st, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(st, 0), "telegram") == 0);
    assert(strcmp((const char *)sqlite3_column_text(st, 1), "telegram") == 0);
    assert(strcmp((const char *)sqlite3_column_text(st, 2), "/usr/local/bin/channel_telegram") == 0);
    assert(strcmp((const char *)sqlite3_column_text(st, 3), "active") == 0);
    sqlite3_finalize(st);
    db_close(db);
}

static void test_channel_events_table(void) {
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    int rc = sqlite3_exec(db,
        "INSERT INTO channel_events(channel_name, event_type, payload) VALUES('telegram','message','{\"chat_id\":123,\"text\":\"hi\"}')",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
    /* FIFO order */
    sqlite3_exec(db,
        "INSERT INTO channel_events(channel_name, event_type, payload) VALUES('telegram','message','{\"chat_id\":123,\"text\":\"second\"}')",
        NULL, NULL, NULL);
    sqlite3_stmt *st;
    rc = sqlite3_prepare_v2(db, "SELECT id, payload FROM channel_events ORDER BY id", -1, &st, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 1);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(sqlite3_column_int(st, 0) == 2);
    sqlite3_finalize(st);
    db_close(db);
}

static void test_channel_outbox_table(void) {
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    int rc = sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name, session_id, payload) VALUES('telegram', 1, '{\"text\":\"hello\"}')",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
    /* Verify default status and index usability */
    sqlite3_stmt *st;
    rc = sqlite3_prepare_v2(db,
        "SELECT id, status FROM channel_outbox WHERE channel_name='telegram' AND status='pending'",
        -1, &st, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(st, 1), "pending") == 0);
    sqlite3_finalize(st);
    /* Simulate ack */
    rc = sqlite3_exec(db,
        "UPDATE channel_outbox SET status='delivered', acked_at=unixepoch() WHERE id=1",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
    db_close(db);
}

static void test_channel_state_table(void) {
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    int rc = sqlite3_exec(db,
        "INSERT INTO channel_state(channel_name, key, value) VALUES('telegram','offset','12345')",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
    /* Upsert (replace) */
    rc = sqlite3_exec(db,
        "INSERT OR REPLACE INTO channel_state(channel_name, key, value) VALUES('telegram','offset','99999')",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_stmt *st;
    rc = sqlite3_prepare_v2(db,
        "SELECT value FROM channel_state WHERE channel_name='telegram' AND key='offset'",
        -1, &st, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(st, 0), "99999") == 0);
    sqlite3_finalize(st);
    db_close(db);
}

int main(void) {
    alarm(10);
    unlink(TEST_DB);
    printf("test_channels_table...");
    test_channels_table();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_channel_events_table...");
    test_channel_events_table();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_channel_outbox_table...");
    test_channel_outbox_table();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_channel_state_table...");
    test_channel_state_table();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("all channel_tables tests passed\n");
    return 0;
}
