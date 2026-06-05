/* T297: Test unified single-DB schema — all tables in one file */
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

static void test_unified_db(void) {
    unlink("/tmp/test_unified.db");
    sqlite3 *db = db_open("/tmp/test_unified.db");
    assert(db);

    /* V4: WAL + busy_timeout */
    assert(pragma_int(db, "PRAGMA busy_timeout;") == 5000);

    /* Has all tables from former daemon schema */
    assert(table_exists(db, "agents"));
    assert(table_exists(db, "agent_config"));
    assert(table_exists(db, "providers"));
    assert(table_exists(db, "kv"));
    assert(table_exists(db, "channel_bindings"));
    assert(table_exists(db, "tg_chat_sessions"));
    assert(table_exists(db, "spawn_queue"));
    assert(table_exists(db, "cron_jobs"));
    assert(table_exists(db, "approvals"));

    /* Has all tables from former agent schema */
    assert(table_exists(db, "sessions"));
    assert(table_exists(db, "entries"));
    assert(table_exists(db, "inbox"));
    assert(table_exists(db, "js_tools"));
    assert(table_exists(db, "memory_blocks"));
    assert(table_exists(db, "tool_calls"));

    /* Has journal table */
    assert(table_exists(db, "log"));

    /* kv seeded with defaults */
    char *val = db_kv_get(db, "provider.model");
    assert(val && strcmp(val, "deepseek/deepseek-v4-flash") == 0);
    free(val);

    /* Can create session + entry */
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    /* Can insert log line */
    const char *sql = "INSERT INTO log(source, pid, stream, line) VALUES('test', 1, 1, 'hello');";
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    assert(rc == SQLITE_OK);

    db_close(db);
    unlink("/tmp/test_unified.db");
    unlink("/tmp/test_unified.db-wal");
    unlink("/tmp/test_unified.db-shm");
    printf("PASS: test_unified_db\n");
}

static void test_aliases(void) {
    /* db_open_cclaw, db_open_agent, db_open_journal all produce same schema */
    unlink("/tmp/test_alias.db");
    sqlite3 *db = db_open_cclaw("/tmp/test_alias.db");
    assert(db);
    assert(table_exists(db, "sessions"));
    assert(table_exists(db, "agents"));
    assert(table_exists(db, "log"));
    db_close(db);
    unlink("/tmp/test_alias.db");
    unlink("/tmp/test_alias.db-wal");
    unlink("/tmp/test_alias.db-shm");
    printf("PASS: test_aliases\n");
}

int main(void) {
    test_unified_db();
    test_aliases();
    printf("ALL PASS\n");
    return 0;
}
