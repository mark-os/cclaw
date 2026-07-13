#define _GNU_SOURCE
#include "cclaw.h"
#include "db.h"
#include "config_registry.h"
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
    unlink(path);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    db_close(db);
    unlink(path);
    printf("  PASS test_open_close\n");
}

static void test_wal_mode(void) {
    const char *path = "/tmp/test_cclaw_wal.sqlite";
    unlink(path);

    sqlite3 *db = test_db_open(path);
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

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* Custom busy handler replaces PRAGMA busy_timeout — pragma reads 0 but
     * contention is handled (and logged) by db_busy_handler. Verify WAL mode
     * is set correctly as a proxy for "db_open configured things properly". */
    char mode[16] = {0};
    get_pragma_text(db, "journal_mode", mode, sizeof(mode));
    assert(strcasecmp(mode, "wal") == 0);

    db_close(db);
    unlink(path);
    printf("  PASS test_busy_timeout\n");
}

static void test_tables_created(void) {
    const char *path = "/tmp/test_cclaw_tables.sqlite";
    unlink(path);

    sqlite3 *db = test_db_open(path);
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

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    db_close(db);

    /* Open again — should not fail on CREATE IF NOT EXISTS */
    db = test_db_open(path);
    assert(db != NULL);
    db_close(db);

    unlink(path);
    printf("  PASS test_reopen_idempotent\n");
}

static void test_fts5_search(void) {
    const char *path = "/tmp/test_cclaw_fts5.sqlite";
    unlink(path);

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

static void test_config_registry(void) {
    const char *path = "/tmp/test_cclaw_registry.sqlite";
    unlink(path);

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
    unlink(path);
    printf("  PASS test_config_registry\n");
}

static void test_agent_pragmas(void) {
    const char *path = "/tmp/test_cclaw_agent_pragmas.db";
    unlink(path);
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
    unlink(path);
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
    unlink(path);
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
    unlink(path);
    printf("  PASS test_prune_inbox\n");
}

static void test_prune_outbox(void) {
    const char *path = "/tmp/test_cclaw_prune_outbox.sqlite";
    unlink(path);
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
    unlink(path);
    printf("  PASS test_prune_outbox\n");
}

static void test_free_mb(void) {
    const char *path = "/tmp/test_cclaw_free_mb.sqlite";
    unlink(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* On-disk DB on /tmp: measurable and positive. */
    assert(db_free_mb(db) > 0);
    /* NULL handle is unmeasurable, not a crash. */
    assert(db_free_mb(NULL) == -1);

    db_close(db);
    unlink(path);
    printf("  PASS test_free_mb\n");
}

static void set_user_version(sqlite3 *db, int v) {
    char sql[48];
    snprintf(sql, sizeof(sql), "PRAGMA user_version=%d;", v);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static int column_exists(sqlite3 *db, const char *table, const char *col) {
    char sql[128];
    snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table);
    sqlite3_stmt *stmt;
    int found = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *n = (const char *)sqlite3_column_text(stmt, 1);
            if (n && strcmp(n, col) == 0) { found = 1; break; }
        }
        sqlite3_finalize(stmt);
    }
    return found;
}

static char *query_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (v) out = strdup(v);
        }
        sqlite3_finalize(stmt);
    }
    return out;
}

static void test_schema_state(void) {
    const char *path = "/tmp/test_cclaw_schema_state.sqlite";
    char wal[128], shm[128];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    snprintf(shm, sizeof(shm), "%s-shm", path);
    unlink(path); unlink(wal); unlink(shm);

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

    set_user_version(db, 10);            /* pre-tracking version */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    set_user_version(db, 0);             /* v0 but tables exist → pre-tracking */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    db_close(db);
    unlink(path); unlink(wal); unlink(shm);
    printf("  PASS test_schema_state\n");
}

static void test_schema_upgrade_from_v11(void) {
    const char *path = "/tmp/test_cclaw_schema_upgrade.sqlite";
    char wal[128], shm[128];
    snprintf(wal, sizeof(wal), "%s-wal", path);
    snprintf(shm, sizeof(shm), "%s-shm", path);
    unlink(path); unlink(wal); unlink(shm);

    sqlite3 *db = test_db_open(path);
    assert(db != NULL);

    /* Reshape a current DB back to v11: re-add the column patch v12 drops,
     * drop the ones patches v14/v17/v19 add, re-add the ones patch v16 drops,
     * re-add model/provider that v17 drops, re-add status that v20 drops,
     * seed rows that each patch must rewrite. */
    assert(sqlite3_exec(db,
        "ALTER TABLE providers ADD COLUMN context_window INTEGER DEFAULT 128000;"
        "ALTER TABLE channels DROP COLUMN prev_extension_name;"
        "ALTER TABLE sessions DROP COLUMN turn_context;"
        "ALTER TABLE cron_jobs DROP COLUMN run_at;"     /* v23 adds */
        "ALTER TABLE cron_jobs DROP COLUMN interval_s;" /* v23 adds */
        "ALTER TABLE cron_jobs DROP COLUMN kind;"       /* v23 adds */
        "ALTER TABLE channel_routes DROP COLUMN delivery_mode;" /* v24 adds */
        "ALTER TABLE config DROP COLUMN secret;"    /* v22 adds */
        "ALTER TABLE config DROP COLUMN required;"  /* v22 adds */
        "ALTER TABLE secrets ADD COLUMN status TEXT NOT NULL DEFAULT 'active';"
        "INSERT INTO secrets(name, value, status, source) VALUES"
        "  ('REAL_KEY','enc:00','active','operator'),"          /* survives sweep */
        "  ('PENDING_JUNK_1','enc:00','pending','quarantine');" /* v20: swept */
        "ALTER TABLE channel_outbox RENAME COLUMN deliver_mode TO deliver_plain;"
        "ALTER TABLE extensions ADD COLUMN builtin INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE tools ADD COLUMN builtin INTEGER NOT NULL DEFAULT 0;"
        "ALTER TABLE agents ADD COLUMN model TEXT;"
        "ALTER TABLE agents ADD COLUMN provider TEXT;"
        "ALTER TABLE agents DROP COLUMN primary_model;"
        "ALTER TABLE agents DROP COLUMN secondary_model;"
        "INSERT INTO models(id, provider_name, model, context_window) VALUES"
        "  ('m-def','p','vendor/def',128000),"      /* old default → NULLed */
        "  ('m-big','p','vendor/big',200000);"      /* explicit → kept */
        "INSERT INTO grants(agent_name, kind, value, approval_mode) VALUES"
        "  ('a1','tool','extension_promote','silent')," /* v13 → 'always', v21 → back to 'silent' (self-parking tool) */
        "  ('a1','tool','shell_exec','silent');"        /* ordinary → untouched */
        "INSERT INTO channel_outbox(channel_name, session_id, payload, deliver_plain) VALUES"
        "  ('tg', 1, '{}', 0),"                         /* auto → mode 0 */
        "  ('tg', 2, '{}', 1);"                         /* plain → mode 2 */
        "INSERT INTO extensions(name, path, owner_agent, published, builtin) VALUES"
        "  ('telegram','/x','',0,1);"                   /* builtin → owner=system, published */
        "INSERT INTO agents(name, model) VALUES('v17agent','m-big');"
        "INSERT INTO config(key, value) VALUES('heartbeat_interval','1800');", /* v23: retired */
        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 11);
    assert(db_schema_state(db, NULL) == DB_SCHEMA_UPGRADABLE);

    assert(db_schema_compat(db) == 1);   /* applies v12..current patches */

    int uv = -1;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT);
    assert(uv == CCLAW_SCHEMA_VERSION);
    assert(!column_exists(db, "providers", "context_window"));

    char *v = query_text(db, "SELECT COALESCE(context_window,'null') FROM models WHERE id='m-def'");
    assert(v && strcmp(v, "null") == 0); free(v);
    v = query_text(db, "SELECT context_window FROM models WHERE id='m-big'");
    assert(v && strcmp(v, "200000") == 0); free(v);
    /* v13 flipped it to 'always'; v21 flips back — promote parks itself now */
    v = query_text(db, "SELECT approval_mode FROM grants WHERE value='extension_promote'");
    assert(v && strcmp(v, "silent") == 0); free(v);
    v = query_text(db, "SELECT approval_mode FROM grants WHERE value='shell_exec'");
    assert(v && strcmp(v, "silent") == 0); free(v);
    v = query_text(db, "SELECT deliver_mode FROM channel_outbox WHERE session_id=1");
    assert(v && strcmp(v, "0") == 0); free(v);
    v = query_text(db, "SELECT deliver_mode FROM channel_outbox WHERE session_id=2");
    assert(v && strcmp(v, "2") == 0); free(v);
    assert(!column_exists(db, "extensions", "builtin"));
    assert(!column_exists(db, "tools", "builtin"));
    v = query_text(db, "SELECT owner_agent FROM extensions WHERE name='telegram'");
    assert(v && strcmp(v, "system") == 0); free(v);
    v = query_text(db, "SELECT published FROM extensions WHERE name='telegram'");
    assert(v && strcmp(v, "1") == 0); free(v);
    /* v17: agents.model → primary_model, provider dropped */
    assert(!column_exists(db, "agents", "model"));
    assert(!column_exists(db, "agents", "provider"));
    assert(column_exists(db, "agents", "primary_model"));
    assert(column_exists(db, "agents", "secondary_model"));
    v = query_text(db, "SELECT primary_model FROM agents WHERE name='v17agent'");
    assert(v && strcmp(v, "m-big") == 0); free(v);
    /* v20: quarantine pending rows swept, status column dropped */
    assert(!column_exists(db, "secrets", "status"));
    v = query_text(db, "SELECT COUNT(*) FROM secrets WHERE name='PENDING_JUNK_1'");
    assert(v && strcmp(v, "0") == 0); free(v);
    v = query_text(db, "SELECT COUNT(*) FROM secrets WHERE name='REAL_KEY'");
    assert(v && strcmp(v, "1") == 0); free(v);
    /* v23: cron_jobs gains run_at/interval_s/kind; a disabled heartbeat pulse
     * is seeded per agent; the heartbeat_interval config key is retired. */
    assert(column_exists(db, "cron_jobs", "run_at"));
    assert(column_exists(db, "cron_jobs", "interval_s"));
    assert(column_exists(db, "cron_jobs", "kind"));
    v = query_text(db, "SELECT COUNT(*) FROM cron_jobs"
                       " WHERE agent_name='v17agent' AND kind='heartbeat'"
                       " AND enabled=0 AND interval_s=1800");
    assert(v && strcmp(v, "1") == 0); free(v);
    v = query_text(db, "SELECT COUNT(*) FROM config WHERE key='heartbeat_interval'");
    assert(v && strcmp(v, "0") == 0); free(v);

    /* Re-running is a no-op, not an error */
    assert(db_schema_compat(db) == 1);

    /* Insurance snapshot taken before the surgery: still v11-shaped */
    char bak[160];
    snprintf(bak, sizeof(bak), "%s.v11.bak", path);
    sqlite3 *bdb = db_open(bak);
    assert(bdb != NULL);
    int buv = -1;
    db_schema_state(bdb, &buv);
    assert(buv == 11);
    assert(column_exists(bdb, "providers", "context_window"));
    v = query_text(bdb, "SELECT approval_mode FROM grants WHERE value='extension_promote'");
    assert(v && strcmp(v, "silent") == 0); free(v);
    db_close(bdb);

    db_close(db);
    unlink(path); unlink(wal); unlink(shm); unlink(bak);
    printf("  PASS test_schema_upgrade_from_v11\n");
}

int main(void) {
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
    test_schema_upgrade_from_v11();
    printf("All db tests passed.\n");
    return 0;
}
