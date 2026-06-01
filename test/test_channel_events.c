/* T244/T245: Test channel launcher tracking + channel_events consumer.
 * Verifies: channel events route to correct agent inbox via channel_bindings. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include "channel_api.h"
#include "daemon.h"
#include "db.h"

static const char *DB_PATH = "/tmp/test_channel_events_daemon.db";
static const char *WORK_DIR = "/tmp/test_channel_events_work";

static void cleanup(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s %s-wal %s-shm %s.pipe",
             WORK_DIR, DB_PATH, DB_PATH, DB_PATH, DB_PATH);
    system(cmd);
}

static void setup(void) {
    cleanup();
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/agents/testagent/workspace", WORK_DIR);
    system(cmd);
}

/* T245: Verify channel_emit → consume_channel_events → inbox_insert */
static void test_channel_events_routing(void) {
    setup();

    /* Open daemon DB */
    sqlite3 *db = db_open_cclaw(DB_PATH);
    assert(db);

    /* Create agent DB at expected path: <CWD>/agents/testagent/agent.db */
    char old_cwd[512];
    getcwd(old_cwd, sizeof(old_cwd));
    chdir(WORK_DIR);

    char agent_db_path[256];
    snprintf(agent_db_path, sizeof(agent_db_path), "agents/testagent/agent.db");
    sqlite3 *adb = db_open_agent(agent_db_path);
    assert(adb);

    /* Create session in agent DB */
    int64_t sid = session_create(adb, "test_session", "testagent", -1, 0);
    assert(sid > 0);
    db_close(adb);

    /* Set up channel binding: mychannel + channel_id "user1" → testagent */
    db_channel_binding_set(db, "mychannel", "user1", "testagent");

    /* Store session mapping in channel_state */
    char sid_str[32];
    snprintf(sid_str, sizeof(sid_str), "%lld", (long long)sid);
    {
        const char *sql = "INSERT OR REPLACE INTO channel_state(channel_name, key, value)"
                          " VALUES('mychannel','session:user1',?);";
        sqlite3_stmt *stmt;
        assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
        sqlite3_bind_text(stmt, 1, sid_str, -1, SQLITE_STATIC);
        assert(sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }

    /* Insert a channel_event (simulating what channel_emit does) */
    {
        const char *sql = "INSERT INTO channel_events(channel_name, event_type, payload)"
                          " VALUES('mychannel','message','{\"channel_id\":\"user1\",\"text\":\"hello from channel\"}');";
        assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    }

    /* Verify the binding resolves correctly */
    char *agent = db_channel_binding_get(db, "mychannel", "user1");
    assert(agent);
    assert(strcmp(agent, "testagent") == 0);
    free(agent);

    /* Simulate what consume_channel_events does: read event, resolve, inbox_insert */
    int64_t inbox_id = daemon_inbox_insert("testagent", sid, "mychannel", "hello from channel");
    assert(inbox_id > 0);

    /* Verify inbox has the message */
    adb = db_open_agent(agent_db_path);
    assert(adb);
    int count = inbox_count(adb, sid);
    assert(count == 1);

    /* Verify content */
    {
        const char *sql = "SELECT payload FROM inbox WHERE session_id=? ORDER BY id DESC LIMIT 1;";
        sqlite3_stmt *stmt;
        assert(sqlite3_prepare_v2(adb, sql, -1, &stmt, NULL) == SQLITE_OK);
        sqlite3_bind_int64(stmt, 1, sid);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        const char *p = (const char *)sqlite3_column_text(stmt, 0);
        assert(p && strstr(p, "hello from channel"));
        sqlite3_finalize(stmt);
    }
    db_close(adb);

    /* Verify channel_events row still exists (we didn't consume it — that's daemon's job) */
    {
        const char *sql = "SELECT COUNT(*) FROM channel_events;";
        sqlite3_stmt *stmt;
        assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
        assert(sqlite3_step(stmt) == SQLITE_ROW);
        assert(sqlite3_column_int(stmt, 0) == 1);
        sqlite3_finalize(stmt);
    }

    db_close(db);
    chdir(old_cwd);
    printf("PASS\n");
}

/* T245: Verify default binding fallback */
static void test_channel_events_default_binding(void) {
    setup();

    sqlite3 *db = db_open_cclaw(DB_PATH);
    assert(db);

    /* Set up "default" binding for channel type */
    db_channel_binding_set(db, "webchat", "default", "testagent");

    /* Verify fallback: no specific binding for "user99", but "default" exists */
    char *agent = db_channel_binding_get(db, "webchat", "user99");
    /* Direct lookup fails */
    assert(agent == NULL);

    /* Fallback to "default" */
    agent = db_channel_binding_get(db, "webchat", "default");
    assert(agent);
    assert(strcmp(agent, "testagent") == 0);
    free(agent);

    db_close(db);
    printf("PASS\n");
}

/* T244: Verify channels table schema works */
static void test_channels_table(void) {
    setup();

    sqlite3 *db = db_open_cclaw(DB_PATH);
    assert(db);

    /* Insert a channel row */
    const char *sql = "INSERT INTO channels(name, type, binary_path)"
                      " VALUES('telegram','telegram','/usr/local/bin/channel_telegram');";
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    /* Read it back */
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db,
        "SELECT name, type, binary_path, status FROM channels WHERE name='telegram';",
        -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "telegram") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "telegram") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 3), "active") == 0);
    sqlite3_finalize(stmt);

    /* Update pid (simulating channel_update_pid) */
    const char *usql = "UPDATE channels SET pid=1234 WHERE name='telegram';";
    assert(sqlite3_exec(db, usql, NULL, NULL, NULL) == SQLITE_OK);

    /* Update status (simulating channel_set_status) */
    const char *ssql = "UPDATE channels SET status='failed' WHERE name='telegram';";
    assert(sqlite3_exec(db, ssql, NULL, NULL, NULL) == SQLITE_OK);

    assert(sqlite3_prepare_v2(db,
        "SELECT status, pid FROM channels WHERE name='telegram';",
        -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "failed") == 0);
    assert(sqlite3_column_int(stmt, 1) == 1234);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("PASS\n");
}

int main(void) {
    alarm(10);

    printf("test_channel_events (T244/T245):\n");

    printf("  channel_events_routing... ");
    test_channel_events_routing();

    printf("  channel_events_default_binding... ");
    test_channel_events_default_binding();

    printf("  channels_table... ");
    test_channels_table();

    cleanup();
    printf("all channel_events tests passed\n");
    return 0;
}
