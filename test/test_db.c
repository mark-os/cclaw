#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

static int get_pragma_int(sqlite3 *db, const char *pragma) {
    char sql[64];
    snprintf(sql, sizeof(sql), "PRAGMA %s;", pragma);
    sqlite3_stmt *stmt;
    int val = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            val = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return val;
}

static int get_pragma_text(sqlite3 *db, const char *pragma, char *buf, int bufsz) {
    char sql[64];
    snprintf(sql, sizeof(sql), "PRAGMA %s;", pragma);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *txt = (const char *)sqlite3_column_text(stmt, 0);
            if (txt) snprintf(buf, (size_t)bufsz, "%s", txt);
        }
        sqlite3_finalize(stmt);
        return 0;
    }
    return -1;
}

static int table_exists(sqlite3 *db, const char *name) {
    const char *sql = "SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?;";
    sqlite3_stmt *stmt;
    int exists = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            exists = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    return exists;
}

static void test_open_close(void) {
    const char *path = "/tmp/test_cclaw_db.sqlite";
    unlink(path);

    sqlite3 *db = db_open(path);
    assert(db != NULL);
    db_close(db);
    unlink(path);
    printf("  PASS test_open_close\n");
}

static void test_wal_mode(void) {
    const char *path = "/tmp/test_cclaw_wal.sqlite";
    unlink(path);

    sqlite3 *db = db_open(path);
    assert(db != NULL);

    char mode[16] = {0};
    get_pragma_text(db, "journal_mode", mode, sizeof(mode));
    assert(strcmp(mode, "wal") == 0);

    db_close(db);
    unlink(path);
    printf("  PASS test_wal_mode\n");
}

static void test_busy_timeout(void) {
    const char *path = "/tmp/test_cclaw_timeout.sqlite";
    unlink(path);

    sqlite3 *db = db_open(path);
    assert(db != NULL);

    /* V4: busy_timeout >= 5000 */
    int timeout = get_pragma_int(db, "busy_timeout");
    assert(timeout >= 5000);

    db_close(db);
    unlink(path);
    printf("  PASS test_busy_timeout\n");
}

static void test_tables_created(void) {
    const char *path = "/tmp/test_cclaw_tables.sqlite";
    unlink(path);

    sqlite3 *db = db_open(path);
    assert(db != NULL);

    assert(table_exists(db, "sessions") == 1);
    assert(table_exists(db, "entries") == 1);

    db_close(db);
    unlink(path);
    printf("  PASS test_tables_created\n");
}

static void test_reopen_idempotent(void) {
    const char *path = "/tmp/test_cclaw_reopen.sqlite";
    unlink(path);

    sqlite3 *db = db_open(path);
    assert(db != NULL);
    db_close(db);

    /* Open again — should not fail on CREATE IF NOT EXISTS */
    db = db_open(path);
    assert(db != NULL);
    db_close(db);

    unlink(path);
    printf("  PASS test_reopen_idempotent\n");
}

static void test_fts5_search(void) {
    const char *path = "/tmp/test_cclaw_fts5.sqlite";
    unlink(path);

    sqlite3 *db = db_open(path);
    assert(db != NULL);

    int64_t sid = session_create(db, "fts_test", NULL, -1, 0);
    assert(sid > 0);

    Message m1 = {.role = ROLE_USER, .content = "hello world from the user"};
    Message m2 = {.role = ROLE_ASSISTANT, .content = "goodbye cruel world"};
    Message m3 = {.role = ROLE_USER, .content = "unrelated message about cats"};
    assert(entry_append(db, sid, &m1) > 0);
    assert(entry_append(db, sid, &m2) > 0);
    assert(entry_append(db, sid, &m3) > 0);

    /* Search for "world" — should find 2 entries */
    int count = 0;
    Entry *results = entry_search(db, "world", sid, &count);
    assert(count == 2);
    assert(results != NULL);
    entry_branch_free(results, count);

    /* Search for "cats" — should find 1 */
    results = entry_search(db, "cats", sid, &count);
    assert(count == 1);
    assert(results != NULL);
    assert(strcmp(results[0].message.content, "unrelated message about cats") == 0);
    entry_branch_free(results, count);

    /* Search for "nonexistent" — should find 0 */
    results = entry_search(db, "nonexistent", sid, &count);
    assert(count == 0);
    assert(results == NULL);

    db_close(db);
    unlink(path);
    printf("  PASS test_fts5_search\n");
}

static void test_kv(void) {
    const char *path = "/tmp/test_cclaw_kv.sqlite";
    unlink(path);

    sqlite3 *db = db_open(path);
    assert(db != NULL);

    /* Get missing key returns NULL */
    char *val = db_kv_get(db, "nope");
    assert(val == NULL);

    /* Set and get */
    assert(db_kv_set(db, "tg_offset", "12345") == 0);
    val = db_kv_get(db, "tg_offset");
    assert(val != NULL);
    assert(strcmp(val, "12345") == 0);
    free(val);

    /* Overwrite */
    assert(db_kv_set(db, "tg_offset", "99999") == 0);
    val = db_kv_get(db, "tg_offset");
    assert(val != NULL);
    assert(strcmp(val, "99999") == 0);
    free(val);

    /* Survives reopen */
    db_close(db);
    db = db_open(path);
    assert(db != NULL);
    val = db_kv_get(db, "tg_offset");
    assert(val != NULL);
    assert(strcmp(val, "99999") == 0);
    free(val);

    db_close(db);
    unlink(path);
    printf("  PASS test_kv\n");
}

static void test_agent_pragmas(void) {
    const char *path = "/tmp/test_cclaw_agent_pragmas.db";
    unlink(path);
    sqlite3 *db = db_open(path);
    assert(db != NULL);

    db_set_agent_pragmas(db);

    /* After: mmap_size=67108864, cache_size=-512 */
    sqlite3_int64 mmap_after = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "PRAGMA mmap_size;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            mmap_after = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    assert(mmap_after == 67108864);

    int cache = get_pragma_int(db, "cache_size");
    assert(cache == -512);

    db_close(db);
    unlink(path);
    printf("  PASS test_agent_pragmas\n");
}

int main(void) {
    printf("test_db:\n");
    test_open_close();
    test_wal_mode();
    test_busy_timeout();
    test_tables_created();
    test_reopen_idempotent();
    test_fts5_search();
    test_kv();
    test_agent_pragmas();
    printf("All db tests passed.\n");
    return 0;
}
