/* T195: Test 3-DB schema openers — V73, V4 */
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int table_exists(sqlite3 *db, const char *name) {
    const char *sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    int found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

static int pragma_int(sqlite3 *db, const char *pragma) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, pragma, -1, &stmt, NULL) != SQLITE_OK) return -1;
    int val = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) val = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return val;
}

static void test_daemon_db(void) {
    unlink("/tmp/test_3db_daemon.db");
    sqlite3 *db = db_open_daemon("/tmp/test_3db_daemon.db");
    assert(db);

    /* V4: WAL + busy_timeout */
    assert(pragma_int(db, "PRAGMA busy_timeout;") == 5000);

    /* Has daemon tables */
    assert(table_exists(db, "agents"));
    assert(table_exists(db, "agent_config"));
    assert(table_exists(db, "providers"));
    assert(table_exists(db, "kv"));
    assert(table_exists(db, "channel_bindings"));
    assert(table_exists(db, "tg_chat_sessions"));
    assert(table_exists(db, "spawn_queue"));
    assert(table_exists(db, "cron_jobs"));
    assert(table_exists(db, "approvals"));

    /* Does NOT have agent tables */
    assert(!table_exists(db, "sessions"));
    assert(!table_exists(db, "entries"));
    assert(!table_exists(db, "inbox"));

    /* Does NOT have journal tables */
    assert(!table_exists(db, "log"));

    /* kv seeded with defaults */
    char *val = db_kv_get(db, "provider.model");
    assert(val && strcmp(val, "deepseek/deepseek-v4-flash") == 0);
    free(val);

    db_close(db);
    unlink("/tmp/test_3db_daemon.db");
    unlink("/tmp/test_3db_daemon.db-wal");
    unlink("/tmp/test_3db_daemon.db-shm");
    printf("PASS: test_daemon_db\n");
}

static void test_agent_db(void) {
    unlink("/tmp/test_3db_agent.db");
    sqlite3 *db = db_open_agent("/tmp/test_3db_agent.db");
    assert(db);

    /* V4: WAL + busy_timeout */
    assert(pragma_int(db, "PRAGMA busy_timeout;") == 5000);

    /* Has agent tables */
    assert(table_exists(db, "sessions"));
    assert(table_exists(db, "entries"));
    assert(table_exists(db, "inbox"));
    assert(table_exists(db, "js_tools"));
    assert(table_exists(db, "memory_blocks"));
    assert(table_exists(db, "kv"));

    /* Does NOT have daemon tables */
    assert(!table_exists(db, "agents"));
    assert(!table_exists(db, "agent_config"));
    /* T202/V73: spawn_queue is daemon.db only */
    assert(!table_exists(db, "spawn_queue"));
    assert(!table_exists(db, "approvals"));

    /* Does NOT have journal tables */
    assert(!table_exists(db, "log"));

    /* Can create session + entry */
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    db_close(db);
    unlink("/tmp/test_3db_agent.db");
    unlink("/tmp/test_3db_agent.db-wal");
    unlink("/tmp/test_3db_agent.db-shm");
    printf("PASS: test_agent_db\n");
}

static void test_journal_db(void) {
    unlink("/tmp/test_3db_journal.db");
    sqlite3 *db = db_open_journal("/tmp/test_3db_journal.db");
    assert(db);

    /* V4: WAL + busy_timeout */
    assert(pragma_int(db, "PRAGMA busy_timeout;") == 5000);

    /* Has log table */
    assert(table_exists(db, "log"));

    /* Does NOT have agent or daemon tables */
    assert(!table_exists(db, "sessions"));
    assert(!table_exists(db, "agents"));
    assert(!table_exists(db, "kv"));

    /* Can insert a log line */
    const char *sql = "INSERT INTO log(source, pid, stream, line) VALUES('test', 1, 1, 'hello');";
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    assert(rc == SQLITE_OK);
    if (err) sqlite3_free(err);

    db_close(db);
    unlink("/tmp/test_3db_journal.db");
    unlink("/tmp/test_3db_journal.db-wal");
    unlink("/tmp/test_3db_journal.db-shm");
    printf("PASS: test_journal_db\n");
}

int main(void) {
    test_daemon_db();
    test_agent_db();
    test_journal_db();
    printf("ALL PASS\n");
    return 0;
}
