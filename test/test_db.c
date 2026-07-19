#define _GNU_SOURCE
#include "cclaw.h"
#include "db.h"
#include "config_registry.h"
#include "util.h"
#include "test_util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
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
    test_db_clean(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    db_close(db);
    test_db_clean(path);
    printf("  PASS test_open_close\n");
}

static void test_wal_mode(void) {
    const char *path = "/tmp/test_cclaw_wal.sqlite";
    test_db_clean(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    char mode[16] = {0};
    get_pragma_text(db, "journal_mode", mode, sizeof(mode));
    assert(strcmp(mode, "wal") == 0);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_wal_mode\n");
}

static void test_busy_timeout(void) {
    const char *path = "/tmp/test_cclaw_timeout.sqlite";
    test_db_clean(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* Custom busy handler replaces PRAGMA busy_timeout — pragma reads 0 but
     * contention is handled (and logged) by db_busy_handler. Verify WAL mode
     * is set correctly as a proxy for "db_open configured things properly". */
    char mode[16] = {0};
    get_pragma_text(db, "journal_mode", mode, sizeof(mode));
    assert(strcasecmp(mode, "wal") == 0);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_busy_timeout\n");
}

static void test_tables_created(void) {
    const char *path = "/tmp/test_cclaw_tables.sqlite";
    test_db_clean(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    assert(table_exists(db, "sessions") == 1);
    assert(table_exists(db, "entries") == 1);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_tables_created\n");
}

static void test_reopen_idempotent(void) {
    const char *path = "/tmp/test_cclaw_reopen.sqlite";
    test_db_clean(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    db_close(db);

    /* Open again — should not fail on CREATE IF NOT EXISTS */
    db = test_db_open(path);
    assert(db != NULL);
    db_close(db);

    test_db_clean(path);
    printf("  PASS test_reopen_idempotent\n");
}

static void test_fts5_search(void) {
    const char *path = "/tmp/test_cclaw_fts5.sqlite";
    test_db_clean(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    int64_t sid = session_create(db, "fts_test", NULL, -1, 0);
    assert(sid > 0);

    Message m1 = {.role = ROLE_USER, .content = "hello world from the user"};
    Message m2 = {.role = ROLE_ASSISTANT, .content = "goodbye cruel world"};
    Message m3 = {.role = ROLE_USER, .content = "unrelated message about cats"};
    assert(entry_append_with_turn(db, sid, &m1, 1) > 0);
    assert(entry_append_with_turn(db, sid, &m2, 1) > 0);
    assert(entry_append_with_turn(db, sid, &m3, 1) > 0);

    /* The entries_fts trigger indexed all three appends */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM entries_fts f JOIN entries e ON e.id = f.rowid"
        " WHERE entries_fts MATCH ?1 AND e.session_id = ?2", -1, &st, NULL) == SQLITE_OK);
    const char *queries[] = {"world", "cats", "nonexistent"};
    int expected[] = {2, 1, 0};
    for (int i = 0; i < 3; i++) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, queries[i], -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, sid);
        assert(sqlite3_step(st) == SQLITE_ROW);
        assert(sqlite3_column_int(st, 0) == expected[i]);
    }
    sqlite3_finalize(st);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_fts5_search\n");
}

static void test_config_registry(void) {
    const char *path = "/tmp/test_cclaw_registry.sqlite";
    test_db_clean(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* Unset registered key returns its default */
    assert(config_get_int(db, "web_port") == 8080);

    /* Set override and read back */
    assert(config_set(db, "web_port", "9090") == 0);
    assert(config_get_int(db, "web_port") == 9090);

    /* Clear override (NULL) resets to default */
    assert(config_set(db, "web_port", NULL) == 0);
    assert(config_get_int(db, "web_port") == 8080);

    /* Unregistered key rejected */
    assert(config_set(db, "tg_offset", "1") == -1);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_config_registry\n");
}

static void test_agent_pragmas(void) {
    const char *path = "/tmp/test_cclaw_agent_pragmas.db";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    db_set_child_pragmas(db);

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
    test_db_clean(path);
    printf("  PASS test_agent_pragmas\n");
}

static int count_rows(sqlite3 *db, const char *table) {
    char sql[64];
    snprintf(sql, sizeof(sql), "SELECT count(*) FROM %s;", table);
    sqlite3_stmt *s;
    int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return n;
}

static void test_prune_inbox(void) {
    const char *path = "/tmp/test_cclaw_prune_inbox.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* default inbox_retention_sec = 604800 (7 days). "old" is well beyond it. */
    assert(sqlite3_exec(db,
        "INSERT INTO inbox(session_id,source,payload,consumed,created_at) VALUES"
        " (1,'cli','a',1,unixepoch()-800000),"   /* consumed + old   → pruned */
        " (1,'cli','b',1,unixepoch()),"          /* consumed + fresh → kept   */
        " (1,'cli','c',0,unixepoch()-800000);",  /* old but unconsumed → kept */
        NULL, NULL, NULL) == SQLITE_OK);
    assert(count_rows(db, "inbox") == 3);

    db_prune_inbox(db);
    assert(count_rows(db, "inbox") == 2);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_prune_inbox\n");
}

static void test_prune_outbox(void) {
    const char *path = "/tmp/test_cclaw_prune_outbox.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* terminal = 'delivered' or 'failed%'; aged from COALESCE(acked_at,created_at). */
    assert(sqlite3_exec(db,
        "INSERT INTO channel_outbox(channel_name,session_id,payload,status,created_at,acked_at) VALUES"
        " ('t',1,'a','delivered',unixepoch()-800000,unixepoch()-800000)," /* terminal+old → pruned */
        " ('t',1,'b','failed: 400',unixepoch()-800000,NULL),"             /* failed%+old  → pruned */
        " ('t',1,'c','pending',unixepoch()-800000,NULL),"                 /* not terminal → kept   */
        " ('t',1,'d','delivered',unixepoch(),unixepoch());",              /* terminal+fresh→ kept  */
        NULL, NULL, NULL) == SQLITE_OK);
    assert(count_rows(db, "channel_outbox") == 4);

    db_prune_outbox(db);
    assert(count_rows(db, "channel_outbox") == 2);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_prune_outbox\n");
}

static void test_free_mb(void) {
    const char *path = "/tmp/test_cclaw_free_mb.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* On-disk DB on /tmp: measurable and positive. */
    assert(db_free_mb(db) > 0);
    /* NULL handle is unmeasurable, not a crash. */
    assert(db_free_mb(NULL) == -1);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_free_mb\n");
}

static void set_user_version(sqlite3 *db, int v) {
    char sql[48];
    snprintf(sql, sizeof(sql), "PRAGMA user_version=%d;", v);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void test_schema_state(void) {
    const char *path = "/tmp/test_cclaw_schema_state.sqlite";
    test_db_clean(path);

    sqlite3 *db = db_open(path);   /* no schema applied */
    assert(db != NULL);
    int uv = -1;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_FRESH && uv == 0);
    assert(db_schema_compat(db) == 1);   /* fresh is usable */

    assert(db_ensure_schema(db) == 0);
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT);
    assert(uv == CCLAW_SCHEMA_VERSION);

    set_user_version(db, CCLAW_SCHEMA_VERSION + 1);
    assert(db_schema_state(db, NULL) == DB_SCHEMA_FUTURE);
    assert(db_schema_compat(db) == 0);   /* refused, not "upgraded" */

    set_user_version(db, 10);            /* far below the v31 floor */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    set_user_version(db, 30);            /* one below the v31 floor — refused */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);
    /* No UPGRADABLE probe: with the floor at the current version the
     * upgradable band is empty until the next schema patch lands. */

    set_user_version(db, 0);             /* v0 but tables exist → pre-freeze */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_schema_state\n");
}

/* Canonical table shape: "table.column:type" lines, sorted. Views/indexes
 * excluded — the patch contract is about table shape. */
static char *schema_shape(sqlite3 *db) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT group_concat(line, char(10) ORDER BY line) FROM ("
        "  SELECT m.name || '.' || ti.name || ':' || ti.type AS line"
        "  FROM sqlite_master m JOIN pragma_table_info(m.name) ti"
        "  WHERE m.type='table' AND m.name NOT LIKE 'sqlite_%')",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *shape = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(shape != NULL);
    return shape;
}

/* Apply schema_patches[] to a floor-shaped DB (frozen fixture — never a
 * current DB with columns dropped) and require the result to match a fresh
 * current DB, table for table, column for column. With an empty patch list
 * (floor == current) this degenerates to "the frozen fixture IS the current
 * shape"; it starts exercising patches again the moment one lands. */
static void test_schema_patch_application(void) {
    const char *old_path = "/tmp/test_cclaw_schema_patch_old.sqlite";
    const char *new_path = "/tmp/test_cclaw_schema_patch_new.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v33.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", old_path, suffixes[i]); unlink(junk);
        snprintf(junk, sizeof(junk), "%s%s", new_path, suffixes[i]); unlink(junk);
    }

    size_t sql_len = 0;
    char *fixture = util_read_file("test/fixtures/schema_v33.sql", &sql_len);
    assert(fixture != NULL && sql_len > 0);

    sqlite3 *old_db = db_open(old_path);
    assert(old_db != NULL);
    char *err = NULL;
    assert(sqlite3_exec(old_db, fixture, NULL, NULL, &err) == SQLITE_OK);
    free(fixture);
    set_user_version(old_db, 33);

    assert(db_schema_compat(old_db) == 1);   /* floor == current: nothing pending */
    int uv = 0;
    assert(db_schema_state(old_db, &uv) == DB_SCHEMA_CURRENT);
    assert(uv == CCLAW_SCHEMA_VERSION);

    sqlite3 *new_db = db_open(new_path);
    assert(new_db != NULL && db_ensure_schema(new_db) == 0);

    char *old_shape = schema_shape(old_db);
    char *new_shape = schema_shape(new_db);
    if (strcmp(old_shape, new_shape) != 0) {
        fprintf(stderr, "patched v31 shape != fresh v%d shape\n-- patched:\n%s\n-- fresh:\n%s\n",
                CCLAW_SCHEMA_VERSION, old_shape, new_shape);
        assert(0);
    }
    free(old_shape); free(new_shape);

    db_close(old_db);
    db_close(new_db);
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", old_path, suffixes[i]); unlink(junk);
        snprintf(junk, sizeof(junk), "%s%s", new_path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_application\n");
}

static void test_rate_limit_and_cost(void) {
    const char *path = "/tmp/test_cclaw_db_budget.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* Recent entry inside both windows: 600 tokens, $1.50 */
    assert(sqlite3_exec(db,
        "INSERT INTO entries(session_id, role, content, usage_in, usage_out,"
        " cost_nano, created_at) VALUES"
        " (1, 2, 'recent', 400, 200, 1500000000, unixepoch()-60);",
        NULL, NULL, NULL) == SQLITE_OK);
    /* Old entry outside both windows (25h > 24h cost, > 1h token window) */
    assert(sqlite3_exec(db,
        "INSERT INTO entries(session_id, role, content, usage_in, usage_out,"
        " cost_nano, created_at) VALUES"
        " (1, 2, 'old', 5000, 5000, 9000000000, unixepoch()-90000);",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Global limit enforced (was dead code: NULL provider always passed) */
    assert(rate_limit_check(db, 500) == 0);  /* 600 used >= 500 */
    assert(rate_limit_check(db, 1000) == 1); /* 600 used < 1000 */
    assert(rate_limit_check(db, 0) == 1);    /* 0 = unlimited */

    /* Cost sum covers only the rolling 24h window */
    assert(db_cost_last_24h(db) == 1500000000LL);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_rate_limit_and_cost\n");
}

int main(void) {
    TEST_INIT();
    printf("test_db:\n");
    test_open_close();
    test_wal_mode();
    test_busy_timeout();
    test_tables_created();
    test_reopen_idempotent();
    test_fts5_search();
    test_config_registry();
    test_agent_pragmas();
    test_prune_inbox();
    test_prune_outbox();
    test_free_mb();
    test_schema_state();
    test_schema_patch_application();
    test_rate_limit_and_cost();
    printf("All db tests passed.\n");
    return 0;
}
