#define _GNU_SOURCE
#include "cclaw.h"
#include "agent_config.h"
#include "db.h"
#include "config_registry.h"
#include "util.h"
#include "test_util.h"
#include "advance.h"
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
    assert(entry_append_with_iteration(db, sid, &m1, 1) > 0);
    assert(entry_append_with_iteration(db, sid, &m2, 1) > 0);
    assert(entry_append_with_iteration(db, sid, &m3, 1) > 0);

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

/* Turn a fresh (current) database back into the v61 shape: hook_directives
 * exists. unshape_v61 starts here, so every older patch test replays v62
 * over a real v61 shape. */
static void unshape_v62(sqlite3 *db) {
    int rc = sqlite3_exec(db,
        "CREATE TABLE hook_directives ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, session_id INTEGER NOT NULL,"
        "  kind TEXT NOT NULL, role TEXT, content TEXT, entry_id INTEGER,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()));"
        "CREATE INDEX idx_hook_directives_session ON hook_directives(session_id);",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
}

/* Turn a fresh (current) database back into the v60 shape: channel_routes
 * instead of channel_chats, the chat columns back on sessions, no declared
 * chat links on sessions/cron_jobs, a NOT NULL policy on every delivery edge.
 * unshape_v60 starts here, so every older patch test replays v61 over a real
 * v60 shape. Rows are carried (a routed chat becomes a route). */
static void unshape_v61(sqlite3 *db) {
    unshape_v62(db);
    char *err = NULL;
    /* legacy_alter_table: the entries triggers name sessions, and a rename
     * onto a just-dropped name would otherwise re-check them mid-swap. */
    int rc = sqlite3_exec(db,
        "PRAGMA foreign_keys=OFF; PRAGMA legacy_alter_table=ON;"
        "CREATE TABLE channel_routes ("
        "  channel_name TEXT NOT NULL, chat_id TEXT NOT NULL,"
        "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
        "  delivery_mode TEXT NOT NULL DEFAULT 'auto', tool_filter TEXT,"
        "  system_prompt_suffix TEXT, PRIMARY KEY (channel_name, chat_id));"
        "INSERT INTO channel_routes SELECT channel_name, chat_id, session_id,"
        "  delivery_mode, tool_filter, guidance FROM channel_chats"
        "  WHERE session_id IS NOT NULL;"
        "CREATE TABLE v60_s ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT,"
        "  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,"
        "  channel_name TEXT, chat_id TEXT, chat_title TEXT, chat_is_admin INTEGER,"
        "  chat_error_notice_at INTEGER, parent_session_id INTEGER DEFAULT -1,"
        "  parent_tool_call_id TEXT, depth INTEGER NOT NULL DEFAULT 0, tool_filter TEXT,"
        "  state TEXT NOT NULL DEFAULT 'idle', owner_instance TEXT,"
        "  turn_iteration INTEGER NOT NULL DEFAULT 0, turn_context TEXT,"
        "  leaf_id INTEGER DEFAULT -1, compaction_fail_count INTEGER NOT NULL DEFAULT 0,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
        "  updated_at INTEGER NOT NULL DEFAULT (unixepoch()));"
        "INSERT INTO v60_s(id, name, agent_name, channel_name, chat_id, parent_session_id,"
        "  parent_tool_call_id, depth, tool_filter, state, owner_instance, turn_iteration,"
        "  turn_context, leaf_id, compaction_fail_count, created_at, updated_at)"
        "  SELECT id, name, agent_name, channel_name, chat_id, parent_session_id,"
        "  parent_tool_call_id, depth, tool_filter, state, owner_instance, turn_iteration,"
        "  turn_context, leaf_id, compaction_fail_count, created_at, updated_at FROM sessions;"
        "DROP TABLE sessions; ALTER TABLE v60_s RENAME TO sessions;"
        "CREATE INDEX idx_sessions_owner ON sessions(owner_instance)"
        "  WHERE owner_instance IS NOT NULL;"
        "DROP TABLE channel_chats;"
        "CREATE TABLE v60_e ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
        "  target_kind TEXT NOT NULL, target_ref TEXT NOT NULL, policy TEXT NOT NULL,"
        "  cursor INTEGER NOT NULL DEFAULT 0, one_shot INTEGER NOT NULL DEFAULT 0,"
        "  UNIQUE(session_id, target_kind, target_ref));"
        "INSERT INTO v60_e SELECT id, session_id, target_kind, target_ref,"
        "  COALESCE(policy,'quiescent'), cursor, one_shot FROM delivery_edges;"
        "DROP TABLE delivery_edges; ALTER TABLE v60_e RENAME TO delivery_edges;"
        "CREATE TABLE v60_c ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,"
        "  name TEXT NOT NULL, cron_expr TEXT NOT NULL, run_at INTEGER, interval_s INTEGER,"
        "  session_id INTEGER NOT NULL, task TEXT NOT NULL, script TEXT,"
        "  channel_name TEXT, chat_id TEXT, target TEXT,"
        "  target_agent TEXT REFERENCES agents(name) ON UPDATE CASCADE,"
        "  enabled INTEGER NOT NULL DEFAULT 1, next_run_at INTEGER NOT NULL DEFAULT 0,"
        "  last_run_at INTEGER, created_at INTEGER NOT NULL DEFAULT (unixepoch()));"
        "INSERT INTO v60_c SELECT * FROM cron_jobs;"
        "DROP TABLE cron_jobs; ALTER TABLE v60_c RENAME TO cron_jobs;"
        "CREATE UNIQUE INDEX idx_cron_jobs_name ON cron_jobs(agent_name, name);"
        "PRAGMA legacy_alter_table=OFF; PRAGMA foreign_keys=ON;", NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "unshape_v61: %s\n", err ? err : "?");
    assert(rc == SQLITE_OK);
}

/* Turn a fresh (current) database back into the v59 shape: no change_log, no
 * generated triggers, grants without source/approval_id. unshape_v59 starts
 * here, so every patch test below v60 replays v60 over a real v59 shape. */
static void unshape_v60(sqlite3 *db) {
    unshape_v61(db);
    char *err = NULL;
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT group_concat('DROP TRIGGER \"' || name || '\";', '')"
        " FROM sqlite_master WHERE type='trigger' AND name LIKE 'log\\_%' ESCAPE '\\'",
        -1, &st, NULL) == SQLITE_OK);
    char *drops = NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0))
        drops = strdup((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    if (drops) assert(sqlite3_exec(db, drops, NULL, NULL, &err) == SQLITE_OK);
    free(drops);
    int rc = sqlite3_exec(db,
        "PRAGMA foreign_keys=OFF;"
        "DROP TABLE change_log;"
        "CREATE TABLE v59_g ("
        "  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE,"
        "  kind TEXT NOT NULL, value TEXT NOT NULL,"
        "  approval_mode TEXT NOT NULL DEFAULT 'silent', expires_at INTEGER,"
        "  created_at INTEGER DEFAULT (unixepoch()),"
        "  PRIMARY KEY (agent_name, kind, value));"
        "INSERT INTO v59_g SELECT agent_name, kind, value, approval_mode, expires_at,"
        "  created_at FROM grants;"
        "DROP TABLE grants; ALTER TABLE v59_g RENAME TO grants;"
        "PRAGMA foreign_keys=ON;", NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "unshape_v60: %s\n", err ? err : "?");
    assert(rc == SQLITE_OK);
}

/* Turn a fresh (current) database back into the v58 shape, so a test that
 * replays patches from an older version runs the v59 step over what a real
 * v58 database looked like: FK-less configuration tables, no purpose, the
 * dropped columns back, memory_blocks.agent_name. Shared by every patch test
 * that starts below v59; their own reverse-shaping runs after this. */
static void unshape_v59(sqlite3 *db) {
    unshape_v60(db);
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "PRAGMA foreign_keys=OFF;"
        "DROP TRIGGER agent_models_capability_bi;"
        "DROP TRIGGER agent_models_capability_bu;"
        "CREATE TABLE v58_am ("
        "  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE ON DELETE CASCADE,"
        "  model_id TEXT NOT NULL REFERENCES models(id),"
        "  pos INTEGER NOT NULL,"
        "  reasoning_effort TEXT CHECK (reasoning_effort IS NULL OR reasoning_effort IN"
        "    ('off','minimal','low','medium','high')),"
        "  PRIMARY KEY (agent_name, pos));"
        "INSERT INTO v58_am SELECT agent_name, model_id, pos, reasoning_effort"
        "  FROM agent_models WHERE purpose='chat';"
        "DROP TABLE agent_models; ALTER TABLE v58_am RENAME TO agent_models;"
        "CREATE TABLE v58_m ("
        "  id TEXT PRIMARY KEY, provider_name TEXT NOT NULL, model TEXT NOT NULL,"
        "  context_window INTEGER, max_output_tokens INTEGER,"
        "  capabilities TEXT DEFAULT '[]', effort_map TEXT,"
        "  status TEXT NOT NULL DEFAULT 'healthy', degraded_until INTEGER,"
        "  total_requests INTEGER DEFAULT 0, total_tokens_in INTEGER DEFAULT 0,"
        "  total_tokens_out INTEGER DEFAULT 0, total_cost_nano INTEGER DEFAULT 0,"
        "  consec_failures INTEGER DEFAULT 0, last_success_at INTEGER,"
        "  last_error_at INTEGER, synced_at INTEGER,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()));"
        "INSERT INTO v58_m SELECT id, provider_name, model, context_window,"
        "  max_output_tokens, capabilities, effort_map, status, degraded_until,"
        "  total_requests, total_tokens_in, total_tokens_out, total_cost_nano,"
        "  consec_failures, last_success_at, last_error_at, synced_at, created_at"
        "  FROM models;"
        "DROP TABLE models; ALTER TABLE v58_m RENAME TO models;"
        "CREATE TABLE v58_e ("
        "  name TEXT PRIMARY KEY, path TEXT NOT NULL, version TEXT DEFAULT '0.0.0',"
        "  owner_agent TEXT, published INTEGER NOT NULL DEFAULT 0,"
        "  enabled INTEGER NOT NULL DEFAULT 1,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()));"
        "INSERT INTO v58_e SELECT name, path, version, owner_agent, published,"
        "  enabled, created_at FROM extensions;"
        "DROP TABLE extensions; ALTER TABLE v58_e RENAME TO extensions;"
        "CREATE TABLE v58_ae ("
        "  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,"
        "  extension_name TEXT NOT NULL, config TEXT,"
        "  enabled INTEGER NOT NULL DEFAULT 1,"
        "  PRIMARY KEY (agent_name, extension_name));"
        "INSERT INTO v58_ae SELECT agent_name, extension_name, config, enabled"
        "  FROM agent_extensions;"
        "DROP TABLE agent_extensions; ALTER TABLE v58_ae RENAME TO agent_extensions;"
        "CREATE TABLE v58_c ("
        "  name TEXT PRIMARY KEY, extension_name TEXT NOT NULL DEFAULT '',"
        "  prev_extension_name TEXT, type TEXT NOT NULL DEFAULT '',"
        "  binary_path TEXT NOT NULL DEFAULT '', status TEXT NOT NULL DEFAULT 'draft',"
        "  pid INTEGER, default_agent TEXT REFERENCES agents(name) ON UPDATE CASCADE,"
        "  default_tool_filter TEXT,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()));"
        "INSERT INTO v58_c(name, extension_name, prev_extension_name, type, status,"
        "                  pid, default_agent, default_tool_filter, created_at)"
        "  SELECT name, COALESCE(extension_name,''), prev_extension_name, type,"
        "         status, pid, default_agent, default_tool_filter, created_at"
        "  FROM channels;"
        "DROP TABLE channels; ALTER TABLE v58_c RENAME TO channels;"
        "CREATE TABLE v58_cs (channel_name TEXT NOT NULL, key TEXT NOT NULL,"
        "  value TEXT, PRIMARY KEY (channel_name, key));"
        "INSERT INTO v58_cs SELECT channel_name, key, value FROM channel_state;"
        "DROP TABLE channel_state; ALTER TABLE v58_cs RENAME TO channel_state;"
        "CREATE TABLE v58_t ("
        "  name TEXT PRIMARY KEY, extension_name TEXT, description TEXT,"
        "  parameters_json TEXT, path TEXT,"
        "  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,"
        "  enabled INTEGER NOT NULL DEFAULT 1, policy TEXT, egress_hosts TEXT);"
        "INSERT INTO v58_t SELECT name, extension_name, description, parameters_json,"
        "  path, agent_name, enabled, policy, egress_hosts FROM tools;"
        "DROP TABLE tools; ALTER TABLE v58_t RENAME TO tools;"
        "CREATE TABLE v58_h (extension_name TEXT NOT NULL, event TEXT NOT NULL,"
        "  path TEXT NOT NULL, enabled INTEGER NOT NULL DEFAULT 1,"
        "  PRIMARY KEY (extension_name, event, path));"
        "INSERT INTO v58_h SELECT extension_name, event, path, enabled FROM hooks;"
        "DROP TABLE hooks; ALTER TABLE v58_h RENAME TO hooks;"
        "CREATE TABLE v58_ad ("
        "  approval_id INTEGER PRIMARY KEY,"
        "  decision    TEXT NOT NULL CHECK (decision IN ('deny','once','always')),"
        "  decided_via TEXT NOT NULL,"
        "  created_at  INTEGER NOT NULL DEFAULT (unixepoch()));"
        "INSERT INTO v58_ad SELECT approval_id, decision, decided_via, created_at"
        "  FROM approval_decisions;"
        "DROP TABLE approval_decisions; ALTER TABLE v58_ad RENAME TO approval_decisions;"
        "ALTER TABLE memory_blocks RENAME COLUMN created_by TO agent_name;"
        "ALTER TABLE providers ADD COLUMN status TEXT NOT NULL DEFAULT 'healthy';"
        "ALTER TABLE llm_jobs ADD COLUMN status TEXT NOT NULL DEFAULT 'pending';"
        "CREATE INDEX idx_llm_jobs_pending ON llm_jobs(status) WHERE status='pending';"
        "ALTER TABLE cron_jobs ADD COLUMN kind TEXT NOT NULL DEFAULT 'task';"
        "PRAGMA foreign_keys=ON;", NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "unshape_v59: %s\n", err ? err : "?");
    assert(rc == SQLITE_OK);
}

/* db_seed_defaults runs on every start, not only on an empty providers table.
 * It used to early-return whenever any provider row existed, so a DB created
 * before a catalog entry was added never saw it — the reason DBs predating the
 * curated provider catalog were missing five providers with no way to get them
 * short of deleting the DB. Seed data is row-shaped, so this is idempotent
 * INSERT OR IGNORE, not a schema patch. */
static void test_seed_backfills_and_preserves(void) {
    const char *path = "/tmp/test_cclaw_seed_backfill.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    assert(db_seed_defaults(db) == 0);

    /* Simulate a DB that predates part of the catalog. */
    assert(sqlite3_exec(db, "DELETE FROM models WHERE provider_name='cerebras';",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "DELETE FROM providers WHERE name='cerebras';",
                        NULL, NULL, NULL) == SQLITE_OK);
    /* ...and an operator edit that must survive re-seeding. */
    assert(sqlite3_exec(db, "UPDATE providers SET default_model='pinned-by-operator'"
                            " WHERE name='openrouter';", NULL, NULL, NULL) == SQLITE_OK);

    assert(db_seed_defaults(db) == 0);   /* second run — no longer a no-op */

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM providers WHERE name='cerebras'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);      /* backfilled */
    sqlite3_finalize(s);

    /* Every catalog provider is routable — a providers row with no models row
     * could never be selected by chat routing. */
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM providers p"
        " WHERE NOT EXISTS (SELECT 1 FROM models m WHERE m.provider_name=p.name)",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 0);
    sqlite3_finalize(s);

    /* INSERT OR IGNORE: the operator's edit won, not the seed's default. */
    assert(sqlite3_prepare_v2(db,
        "SELECT default_model FROM providers WHERE name='openrouter'",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "pinned-by-operator") == 0);
    sqlite3_finalize(s);

    db_close(db);
    test_db_clean(path);
    printf("  PASS: test_seed_backfills_and_preserves\n");
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

    set_user_version(db, 10);            /* far below the v40 floor */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    set_user_version(db, 39);            /* one below the v40 floor — refused */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    /* At the floor: inside the patchable band. Classification only — running
     * the upgrade here would re-ALTER a DB that already has the v41 shape;
     * test_schema_patch_application does that against a real v40 fixture. */
    set_user_version(db, 40);
    assert(db_schema_state(db, NULL) == DB_SCHEMA_UPGRADABLE);

    set_user_version(db, 0);             /* v0 but tables exist → pre-freeze */
    assert(db_schema_state(db, NULL) == DB_SCHEMA_TOO_OLD);
    assert(db_schema_compat(db) == 0);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_schema_state\n");
}

/* Canonical shape: "table.column:type notnull pk" lines, every declared
 * foreign key, and the name of every table/index/trigger, sorted. Compared
 * by structure rather than sqlite_master.sql text, which comments and
 * IF NOT EXISTS would make differ between schema.sql and a patch. */
static char *schema_shape(sqlite3 *db) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT group_concat(line, char(10) ORDER BY line) FROM ("
        "  SELECT m.name || '.' || ti.name || ':' || ti.type || ' nn' || ti.\"notnull\""
        "         || ' pk' || ti.pk AS line"
        "  FROM sqlite_master m JOIN pragma_table_info(m.name) ti"
        "  WHERE m.type='table' AND m.name NOT LIKE 'sqlite_%'"
        "  UNION ALL"
        "  SELECT 'fk ' || m.name || '.' || fk.\"from\" || ' -> ' || fk.\"table\" || '.'"
        "         || COALESCE(fk.\"to\",'') || ' ' || fk.on_update || '/' || fk.on_delete"
        "  FROM sqlite_master m JOIN pragma_foreign_key_list(m.name) fk"
        "  WHERE m.type='table' AND m.name NOT LIKE 'sqlite_%'"
        "  UNION ALL"
        "  SELECT m.type || ' ' || m.name || ' on ' || m.tbl_name FROM sqlite_master m"
        "  WHERE m.name NOT LIKE 'sqlite_%' AND m.type IN ('table','index','trigger')"
        "    AND m.name NOT LIKE 'entries_fts_%'"
        /* Index and trigger bodies, whitespace-folded: a partial index's WHERE
         * or a trigger's logic is not visible in the lines above. */
        "  UNION ALL"
        "  SELECT 'sql ' || m.name || ': ' || lower(replace(replace(replace(replace("
        "         replace(replace(m.sql, char(10), ' '), char(9), ' '), '    ', ' '),"
        "         '  ', ' '), '  ', ' '), ' if not exists', ''))"
        "  FROM sqlite_master m WHERE m.type IN ('index','trigger') AND m.sql IS NOT NULL)",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *shape = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(shape != NULL);
    return shape;
}

/* Apply schema_patches[] to a floor-shaped DB (frozen fixture — never a
 * current DB with columns dropped) and require the result to match a fresh
 * current DB, table for table, column for column. This is what catches a
 * schema.sql change that shipped without the matching ALTER in the patch. */
static void test_schema_patch_application(void) {
    const char *old_path = "/tmp/test_cclaw_schema_patch_old.sqlite";
    const char *new_path = "/tmp/test_cclaw_schema_patch_new.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v40.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", old_path, suffixes[i]); unlink(junk);
        snprintf(junk, sizeof(junk), "%s%s", new_path, suffixes[i]); unlink(junk);
    }

    size_t sql_len = 0;
    char *fixture = util_read_file("test/fixtures/schema_v40.sql", &sql_len);
    assert(fixture != NULL && sql_len > 0);

    sqlite3 *old_db = db_open(old_path);
    assert(old_db != NULL);
    char *err = NULL;
    assert(sqlite3_exec(old_db, fixture, NULL, NULL, &err) == SQLITE_OK);
    free(fixture);
    /* A legacy pulse row: v41 folds kind='heartbeat' into a bare-wake job. */
    assert(sqlite3_exec(old_db,
        "INSERT INTO agents(name) VALUES('Pulse');"
        "INSERT INTO cron_jobs(agent_name,name,kind,cron_expr,interval_s,"
        " session_id,task,enabled,next_run_at)"
        " VALUES('Pulse','heartbeat','heartbeat','',1800,0,'',1,0);",
        NULL, NULL, NULL) == SQLITE_OK);
    /* Delivery-edge migration inputs (v42): a finished background child —
     * the v41 backfill stamps it delivered, so its cursor lands at the leaf —
     * and an in-flight blocking child whose call is still running, which
     * gets its one-shot reply edge. */
    assert(sqlite3_exec(old_db,
        "INSERT INTO sessions(id, name, agent_name, parent_session_id, depth)"
        " VALUES (100,'p','Pulse',-1,0), (101,'c','Pulse',100,1),"
        "        (102,'b','Pulse',100,1);"
        "UPDATE sessions SET parent_tool_call_id='call_m' WHERE id=102;"
        "INSERT INTO entries(id, session_id, role, content) VALUES(500,101,2,'done');"
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
        " VALUES(100, 1, 'call_m', 'launch_agent', 'running');",
        NULL, NULL, NULL) == SQLITE_OK);
    /* v44 model-id migration inputs: one unambiguous bare name, one that
     * matches nothing, and one carried by two providers. */
    assert(sqlite3_exec(old_db,
        "INSERT INTO providers(name, base_url, priority) VALUES"
        " ('p1','http://p1',0), ('lo','http://lo',5), ('hi','http://hi',1);"
        "INSERT INTO models(id, provider_name, model, priority) VALUES"
        " ('solo@p1','p1','solo',0),"
        " ('dup@lo','lo','dup',5),"
        " ('dup@hi','hi','dup',1);"
        "INSERT INTO agents(name, primary_model, secondary_model) VALUES"
        " ('Uniq','solo','dup'), ('Zero','nowhere',NULL),"
        " ('Canon','solo@p1',NULL);",
        NULL, NULL, NULL) == SQLITE_OK);
    /* v47 inputs: approvals rows carrying the old dual-purpose `action`
     * (a tool name, a document verb, and the 'sensitive' overlay). The
     * cron/lease half of v47 is exercised from a v46 fixture below, where
     * the columns it touches already exist. */
    assert(sqlite3_exec(old_db,
        "INSERT INTO approvals(session_id, tool_name, action, state) VALUES"
        " (100,'web_fetch','sensitive','pending'),"
        " (100,'shell_exec','shell_exec','pending'),"
        " (100,'request_config','request_changes','denied');",
        NULL, NULL, NULL) == SQLITE_OK);
    /* v53 inputs: tool results stored with an empty tool_name — one whose
     * call is present, one whose call is not — and a named one left alone. */
    assert(sqlite3_exec(old_db,
        "INSERT INTO entries(id, session_id, iteration_id, type, role, tool_call_id, tool_name)"
        " VALUES (600,100,77,'tool_call',2,'call_a','shell_exec'),"
        "        (601,100,77,'tool_call',2,'call_b','js_eval'),"
        "        (602,100,77,'tool_result',3,'call_b',''),"
        "        (603,100,77,'tool_result',3,'call_a',''),"
        "        (604,100,78,'tool_result',3,'call_gone',''),"
        "        (605,100,77,'tool_result',3,'call_a','kept');",
        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(old_db, 40);

    assert(db_schema_compat(old_db) == 1);   /* runs every pending patch */

    /* v53: each empty name becomes its call's name; no call → NULL. */
    assert(db_scalar_i64(old_db,
        "SELECT (SELECT tool_name FROM entries WHERE id=602)='js_eval'"
        "   AND (SELECT tool_name FROM entries WHERE id=603)='shell_exec'"
        "   AND (SELECT tool_name FROM entries WHERE id=604) IS NULL"
        "   AND (SELECT tool_name FROM entries WHERE id=605)='kept';", 0, -1) == 1);
    assert(db_scalar_i64(old_db,
        "SELECT COUNT(*) FROM entries WHERE type='tool_result' AND tool_name='';",
        0, -1) == 0);

    /* v47 rekey: 'sensitive' becomes the overlay value, everything else —
     * tool names and document verbs alike — is an ordinary required park.
     * tool_name is untouched: it is now the sole answer to "what parked". */
    assert(db_scalar_i64(old_db,
        "SELECT COUNT(*) FROM approvals WHERE park_reason='sensitive_target'"
        " AND tool_name='web_fetch';", 0, -1) == 1);
    assert(db_scalar_i64(old_db,
        "SELECT COUNT(*) FROM approvals WHERE park_reason='approval_required';",
        0, -1) == 2);


    /* v44 canonicalized the scalars (unambiguous → rewritten; ambiguous →
     * lowest models.priority; no match → left alone), then v46 folded them
     * into agent_models rows — an unresolvable scalar yields NO row (that
     * agent was already unroutable; inventing an id would move it somewhere
     * nobody chose). */
    sqlite3_stmt *mm;
    assert(sqlite3_prepare_v2(old_db,
        "SELECT agent_name, model_id, pos FROM agent_models"
        " WHERE agent_name IN ('Uniq','Zero','Canon')"
        " ORDER BY agent_name, pos",
        -1, &mm, NULL) == SQLITE_OK);
    assert(sqlite3_step(mm) == SQLITE_ROW);           /* Canon pos 0 */
    assert(strcmp((const char *)sqlite3_column_text(mm, 1), "solo@p1") == 0);
    assert(sqlite3_column_int(mm, 2) == 0);
    assert(sqlite3_step(mm) == SQLITE_ROW);           /* Uniq pos 0 */
    assert(strcmp((const char *)sqlite3_column_text(mm, 1), "solo@p1") == 0);
    assert(sqlite3_step(mm) == SQLITE_ROW);           /* Uniq pos 1 */
    assert(strcmp((const char *)sqlite3_column_text(mm, 1), "dup@hi") == 0);
    assert(sqlite3_column_int(mm, 2) == 1);
    assert(sqlite3_step(mm) == SQLITE_DONE);          /* Zero: no rows */
    sqlite3_finalize(mm);
    int uv = 0;
    assert(db_schema_state(old_db, &uv) == DB_SCHEMA_CURRENT);
    assert(uv == CCLAW_SCHEMA_VERSION);

    /* Converted in place, enabled kept — an operator who turned the pulse on
     * keeps it running, now as an ordinary bare-wake job (and v59 dropped the
     * kind column that once told them apart). */
    sqlite3_stmt *hb;
    assert(sqlite3_prepare_v2(old_db,
        "SELECT task, cron_expr, interval_s, enabled FROM cron_jobs"
        " WHERE agent_name='Pulse' AND name='heartbeat'", -1, &hb, NULL) == SQLITE_OK);
    assert(sqlite3_step(hb) == SQLITE_ROW);
    assert(sqlite3_column_text(hb, 0)[0] == '\0');
    assert(strcmp((const char *)sqlite3_column_text(hb, 1), "*/30 * * * *") == 0);
    assert(sqlite3_column_type(hb, 2) == SQLITE_NULL);
    assert(sqlite3_column_int(hb, 3) == 1);
    sqlite3_finalize(hb);
    assert(db_scalar_i64(old_db, "SELECT COUNT(*) FROM pragma_table_info('cron_jobs')"
        " WHERE name='kind';", 0, -1) == 0);

    /* v42: every pre-existing child got a standing parent edge at 'turn' (the
     * contract it was launched under); the delivered child's cursor sits at
     * its leaf, the blocking child also carries its one-shot reply edge, and
     * the stamp column is gone. */
    sqlite3_stmt *de;
    assert(sqlite3_prepare_v2(old_db,
        "SELECT session_id, target_kind, target_ref, policy, cursor, one_shot"
        " FROM delivery_edges ORDER BY session_id, one_shot;", -1, &de, NULL)
        == SQLITE_OK);
    assert(sqlite3_step(de) == SQLITE_ROW);   /* 101 parent, delivered */
    assert(sqlite3_column_int64(de, 0) == 101);
    assert(strcmp((const char *)sqlite3_column_text(de, 1), "parent") == 0);
    assert(strcmp((const char *)sqlite3_column_text(de, 2), "100") == 0);
    assert(strcmp((const char *)sqlite3_column_text(de, 3), "turn") == 0);
    assert(sqlite3_column_int64(de, 4) == 500);
    assert(sqlite3_column_int(de, 5) == 0);
    assert(sqlite3_step(de) == SQLITE_ROW);   /* 102 parent, nothing yet */
    assert(sqlite3_column_int64(de, 0) == 102);
    assert(sqlite3_column_int64(de, 4) == 0);
    assert(sqlite3_column_int(de, 5) == 0);
    assert(sqlite3_step(de) == SQLITE_ROW);   /* 102 one-shot reply edge */
    assert(sqlite3_column_int64(de, 0) == 102);
    assert(strcmp((const char *)sqlite3_column_text(de, 1), "tool_call") == 0);
    assert(strcmp((const char *)sqlite3_column_text(de, 2), "call_m") == 0);
    assert(sqlite3_column_int(de, 5) == 1);
    assert(sqlite3_step(de) == SQLITE_DONE);
    sqlite3_finalize(de);
    assert(db_scalar_i64(old_db,
        "SELECT COUNT(*) FROM pragma_table_info('sessions')"
        " WHERE name='parent_notified_at';", 0, -1) == 0);

    sqlite3 *new_db = db_open(new_path);
    assert(new_db != NULL && db_ensure_schema(new_db) == 0);

    char *old_shape = schema_shape(old_db);
    char *new_shape = schema_shape(new_db);
    if (strcmp(old_shape, new_shape) != 0) {
        fprintf(stderr, "patched v40 shape != fresh v%d shape\n-- patched:\n%s\n-- fresh:\n%s\n",
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

/* The v46→v47 step in isolation: a fixture reverse-shaped to v46 (the
 * columns v47 rewrites all exist there), upgraded, then checked for the
 * three shape changes and the two data migrations. */
static void test_schema_patch_v47(void) {
    const char *path = "/tmp/test_cclaw_schema_v47.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v46.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v59(db);

    /* Reverse-shape to v46: FK-less cron_jobs, no lease columns, the old
     * dual-purpose approvals.action. */
    assert(sqlite3_exec(db,
        "PRAGMA foreign_keys=OFF;"
        /* v48/v49/v50 are above this step: shed their columns too, or the
         * upgrade run below re-adds ones that already exist. */
        "ALTER TABLE entries DROP COLUMN reasoning_meta;"
        "ALTER TABLE entries DROP COLUMN cached_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cached_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cache_write_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN reasoning_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cost;"
        "ALTER TABLE providers DROP COLUMN request_extra;"
        "ALTER TABLE llm_responses DROP COLUMN upstream_provider;"
        "ALTER TABLE sessions DROP COLUMN chat_is_admin;"
        "ALTER TABLE sessions DROP COLUMN chat_error_notice_at;"
        "ALTER TABLE agent_models DROP COLUMN reasoning_effort;"
        "ALTER TABLE models DROP COLUMN effort_map;"
        "ALTER TABLE approvals RENAME COLUMN park_reason TO action;"
        "ALTER TABLE agents DROP COLUMN hold_until;"
        "ALTER TABLE agents DROP COLUMN hold_holder;"
        "CREATE TABLE cj46 ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  agent_name TEXT REFERENCES agents(name) ON UPDATE CASCADE,"
        "  name TEXT NOT NULL, cron_expr TEXT NOT NULL, run_at INTEGER,"
        "  interval_s INTEGER, kind TEXT NOT NULL DEFAULT 'task',"
        "  session_id INTEGER NOT NULL, task TEXT NOT NULL, script TEXT,"
        "  channel_name TEXT, chat_id TEXT, target TEXT, target_agent TEXT,"
        "  enabled INTEGER NOT NULL DEFAULT 1,"
        "  next_run_at INTEGER NOT NULL DEFAULT 0, last_run_at INTEGER,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()));"
        "DROP TABLE cron_jobs;"
        "ALTER TABLE cj46 RENAME TO cron_jobs;"
        "CREATE UNIQUE INDEX idx_cron_jobs_name ON cron_jobs(agent_name, name);"
        "PRAGMA foreign_keys=ON;", NULL, NULL, NULL) == SQLITE_OK);

    assert(sqlite3_exec(db,
        "INSERT INTO agents(name) VALUES('Keep');"
        "INSERT INTO approvals(session_id, tool_name, action, state) VALUES"
        " (1,'web_fetch','sensitive','pending'), (1,'js_eval','js_eval','pending');"
        "INSERT INTO cron_jobs(agent_name,name,cron_expr,session_id,task,"
        " target,target_agent) VALUES"
        " ('Keep','orphan','* * * * *',0,'x','new','GhostAgent'),"
        " ('Keep','keeper','* * * * *',0,'x','new','Keep');",
        NULL, NULL, NULL) == SQLITE_OK);
    /* v58 column, absent from every earlier shape */
    assert(sqlite3_exec(db, "ALTER TABLE entries DROP COLUMN fence_id;",
                        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 46);

    assert(db_schema_compat(db) == 1);
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT);

    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM approvals WHERE park_reason='sensitive_target'"
        " AND tool_name='web_fetch';", 0, -1) == 1);
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM approvals WHERE park_reason='approval_required'"
        " AND tool_name='js_eval';", 0, -1) == 1);
    /* The already-broken row is NULLed (it named nothing the FK could
     * accept); the good one survives the table rebuild... */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM cron_jobs WHERE name='orphan'"
        " AND target_agent IS NULL;", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM cron_jobs WHERE name='keeper'"
        " AND target_agent='Keep';", 0, -1) == 1);
    /* ...with its index and its new FK, so a rename now cascades into it. */
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index'"
        " AND name='idx_cron_jobs_name';", 0, -1) == 1);
    assert(agent_rename(db, "Keep", "Kept", 0) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM cron_jobs"
        " WHERE target_agent='Kept';", 0, -1) == 1);
    /* Lease columns land empty — nothing is held on a fresh upgrade. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM agents"
        " WHERE hold_until IS NOT NULL OR hold_holder IS NOT NULL;", 0, -1) == 0);

    db_close(db);
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_v47\n");
}

/* The v47->v48 step in isolation: reasoning replay's one column lands on
 * entries, nullable and empty (no backfill is possible — old reasoning rows
 * have no captured blob and are simply never replayed). */
static void test_schema_patch_v48(void) {
    const char *path = "/tmp/test_cclaw_schema_v48.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm" };
    for (size_t i = 0; i < 3; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v59(db);

    assert(sqlite3_exec(db,
        "ALTER TABLE entries DROP COLUMN reasoning_meta;"
        /* v49/v50/v51 sit above this step: shed their columns too. */
        "ALTER TABLE sessions DROP COLUMN chat_is_admin;"
        "ALTER TABLE sessions DROP COLUMN chat_error_notice_at;"
        "ALTER TABLE agent_models DROP COLUMN reasoning_effort;"
        "ALTER TABLE models DROP COLUMN effort_map;"
        "ALTER TABLE entries DROP COLUMN cached_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cached_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cache_write_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN reasoning_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cost;"
        "ALTER TABLE providers DROP COLUMN request_extra;"
        "ALTER TABLE llm_responses DROP COLUMN upstream_provider;",
        NULL, NULL, NULL) == SQLITE_OK);
    /* v58 column, absent from every earlier shape */
    assert(sqlite3_exec(db, "ALTER TABLE entries DROP COLUMN fence_id;",
                        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 47);

    assert(db_schema_compat(db) == 1);
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT);
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM pragma_table_info('entries')"
        " WHERE name='reasoning_meta';", 0, -1) == 1);
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM entries WHERE reasoning_meta IS NOT NULL;", 0, -1) == 0);

    db_close(db);
    for (size_t i = 0; i < 3; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_v48\n");
}

/* v50 (provider wire-format M1): the two observability/config columns land,
 * and the seed provider gets the StreamLake exclusion — but only when the
 * operator hasn't already written a request_extra of their own. */
static void test_schema_patch_v50(void) {
    const char *path = "/tmp/test_cclaw_db_v50.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    unshape_v59(db);

    assert(sqlite3_exec(db,
        "ALTER TABLE providers DROP COLUMN request_extra;"
        "ALTER TABLE llm_responses DROP COLUMN upstream_provider;"
        /* v51 sits above this step: shed its columns too. */
        "ALTER TABLE sessions DROP COLUMN chat_is_admin;"
        "ALTER TABLE sessions DROP COLUMN chat_error_notice_at;"
        "ALTER TABLE agent_models DROP COLUMN reasoning_effort;"
        "ALTER TABLE models DROP COLUMN effort_map;"
        "INSERT OR IGNORE INTO providers(name, base_url)"
        " VALUES('openrouter','https://openrouter.ai/api/v1'),('custom','http://x');",
        NULL, NULL, NULL) == SQLITE_OK);
    /* v58 column, absent from every earlier shape */
    assert(sqlite3_exec(db, "ALTER TABLE entries DROP COLUMN fence_id;",
                        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 49);

    assert(db_schema_compat(db) == 1);
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT);

    assert(db_scalar_i64(db,
        "SELECT json_extract(request_extra,'$.provider.ignore[0]')='StreamLake'"
        " FROM providers WHERE name='openrouter';", 0, -1) == 1);
    /* Every other provider stays unopinionated. */
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM providers WHERE name='custom'"
        " AND request_extra IS NULL;", 0, -1) == 1);
    /* Column exists and is writable on the archive table. */
    assert(sqlite3_exec(db,
        "INSERT INTO llm_responses(session_id,iteration_id,status,upstream_provider)"
        " VALUES(1,1,'ok','StreamLake');", NULL, NULL, NULL) == SQLITE_OK);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_schema_patch_v50\n");
}

/* v51 (provider wire-format M5): the reasoning-effort knob lands on the
 * routing entry and its wire spelling on the model, both NULL — so an
 * upgraded DB sends exactly what it sent before. */
static void test_schema_patch_v51(void) {
    const char *path = "/tmp/test_cclaw_db_v51.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    unshape_v59(db);

    assert(sqlite3_exec(db,
        "ALTER TABLE sessions DROP COLUMN chat_is_admin;"
        "ALTER TABLE sessions DROP COLUMN chat_error_notice_at;"
        "ALTER TABLE agent_models DROP COLUMN reasoning_effort;"
        "ALTER TABLE models DROP COLUMN effort_map;"
        "INSERT INTO agents(name) VALUES('Router');"
        "INSERT INTO providers(name, base_url) VALUES('p','http://p');"
        "INSERT INTO models(id, provider_name, model)"
        " VALUES('m@p','p','m');"
        "INSERT INTO agent_models(agent_name, model_id, pos)"
        " VALUES('Router','m@p',0);", NULL, NULL, NULL) == SQLITE_OK);
    /* v58 column, absent from every earlier shape */
    assert(sqlite3_exec(db, "ALTER TABLE entries DROP COLUMN fence_id;",
                        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 50);

    assert(db_schema_compat(db) == 1);
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT);

    /* Both columns exist, and the pre-existing routing entry is untouched. */
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM pragma_table_info('agent_models')"
        " WHERE name='reasoning_effort';", 0, -1) == 1);
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM pragma_table_info('models')"
        " WHERE name='effort_map';", 0, -1) == 1);
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM agent_models WHERE reasoning_effort IS NULL;",
        0, -1) == 1);

    /* The level vocabulary is enforced by the patched-in CHECK, not just by
     * whoever writes the row. */
    assert(sqlite3_exec(db, "UPDATE agent_models SET reasoning_effort='high';",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE agent_models SET reasoning_effort='ludicrous';",
                        NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db,
        "UPDATE models SET effort_map='{\"format\":\"openai\",\"levels\":{}}';",
        NULL, NULL, NULL) == SQLITE_OK);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_schema_patch_v51\n");
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

/* v56 -> v57: memory_entries becomes memory_notes (ids in display order),
 * every block is attached to its owner, and the renamed tools carry their
 * grants, the stored default toolset and the tools rows with them. */
static int refuse_commit(void *unused) { (void)unused; return 1; }

static void test_schema_patch_v57(void) {
    const char *path = "/tmp/test_cclaw_schema_v57.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v56.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v59(db);

    /* Reverse-shape to v56. Entries are inserted so that id order and pos
     * order DISAGREE for agent A: the patch must follow pos. */
    assert(sqlite3_exec(db,
        "DROP TABLE memory_notes; DROP TABLE agent_memory_blocks;"
        "CREATE TABLE memory_entries ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  agent_name TEXT NOT NULL REFERENCES agents(name) ON UPDATE CASCADE,"
        "  block_label TEXT NOT NULL, pos INTEGER NOT NULL, text TEXT NOT NULL,"
        "  created_at INTEGER NOT NULL DEFAULT (unixepoch()));"
        "CREATE INDEX idx_memory_entries_block"
        "  ON memory_entries(agent_name, block_label, pos);"
        "INSERT INTO agents(name, system_prompt) VALUES"
        " ('A','Save it with memory_edit. Tools: memory_add, memory_delete, memory_create.'),"
        " ('B','no tools named here');"
        "DELETE FROM memory_blocks;"
        "INSERT INTO memory_blocks(agent_name,label,placement) VALUES"
        " ('A','NOTES','system'),('B','NOTES','context'),('B','EMPTY','context');"
        "INSERT INTO memory_entries(agent_name,block_label,pos,text) VALUES"
        " ('A','NOTES',2,'a-second'),('B','NOTES',1,'use memory_edit here'),('A','NOTES',1,'a-first'),"
        " ('A','GONE',1,'orphan of a block that no longer exists');"
        "UPDATE memory_entries SET created_at=1700000000;"
        "DELETE FROM grants; DELETE FROM tools WHERE name LIKE 'memory_%';"
        "INSERT INTO grants(agent_name,kind,value) VALUES"
        " ('A','tool','memory_create'),('A','tool','memory_add'),"
        " ('A','tool','memory_edit'),('A','tool','memory_delete'),"
        " ('A','tool','file_read'),('B','tool','memory_add');"
        "INSERT INTO tools(name,description) VALUES('memory_create','x'),"
        " ('memory_add','x'),('memory_edit','x'),('memory_delete','x');"
        "INSERT OR REPLACE INTO config(key,value) VALUES('agent_default_tools',"
        " '[\"file_read\",\"memory_create\",\"memory_add\",\"memory_edit\",\"memory_delete\"]');",
        NULL, NULL, NULL) == SQLITE_OK);
    /* v58: a result tagged before the column existed gets its fence id. */
    assert(sqlite3_exec(db,
        "ALTER TABLE entries DROP COLUMN fence_id;"
        "INSERT INTO entries(session_id, role, content, network_hosts) VALUES"
        " (1, 3, 'fetched', '[\"example.com\"]'), (1, 3, 'local', NULL);",
        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 56);

    /* A patch whose COMMIT fails did not happen: the upgrade must report
     * failure and leave the DB at the version it started from, not log
     * "upgraded" and hand a v56-shaped database to a v57 build. */
    sqlite3_commit_hook(db, refuse_commit, NULL);
    assert(db_schema_compat(db) == 0);
    sqlite3_commit_hook(db, NULL, NULL);
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_UPGRADABLE && uv == 56);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM memory_entries;", 0, -1) == 4);

    assert(db_schema_compat(db) == 1);
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE content='fetched'"
        " AND length(fence_id)=16;", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE content='local'"
        " AND fence_id IS NULL;", 0, -1) == 1);

    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sqlite_master"
        " WHERE name IN ('memory_entries','idx_memory_entries_block');", 0, -1) == 0);
    /* Notes land on the right block, in pos order, orphan dropped. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM memory_notes;", 0, -1) == 3);
    assert(db_scalar_i64(db,
        "SELECT group_concat(n.text, ',' ORDER BY n.id) = 'a-first,a-second'"
        " FROM memory_notes n JOIN memory_blocks b ON b.id=n.block_id"
        " WHERE b.created_by='A' AND b.label='NOTES';", 0, -1) == 1);
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM memory_notes n JOIN memory_blocks b ON b.id=n.block_id"
        " WHERE b.created_by='B' AND b.label='NOTES'"
        "   AND n.text='use memory_note_edit here';", 0, -1) == 1);
    /* Stored prompts follow the rename too. */
    assert(db_scalar_i64(db, "SELECT system_prompt = 'Save it with memory_note_edit. Tools: "
        "memory_note_add, memory_note_delete, memory_block_create.'"
        " FROM agents WHERE name='A';", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT system_prompt = 'no tools named here'"
        " FROM agents WHERE name='B';", 0, -1) == 1);
    /* A migrated note's "last changed" is its creation time. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM memory_notes"
        " WHERE updated_at=created_at AND created_at=1700000000;", 0, -1) == 3);
    /* Every block attached read-write to its owner — and to nobody else. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM agent_memory_blocks a"
        " JOIN memory_blocks b ON b.id=a.block_id"
        " WHERE a.agent_name=b.created_by AND a.read_only=0;", 0, -1) == 3);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM agent_memory_blocks;", 0, -1) == 3);
    /* The API sees the migrated state. */
    MemoryBlock *mb = memory_block_get(db, "A", "NOTES");
    assert(mb && mb->read_only == 0 && strcmp(mb->placement, "system") == 0);
    memory_block_free(mb);

    /* Grants follow the rename; whoever could create blocks can update them. */
    assert(db_scalar_i64(db, "SELECT group_concat(value, ',' ORDER BY value) ="
        " 'file_read,memory_block_create,memory_block_update,memory_note_add,"
        "memory_note_delete,memory_note_edit'"
        " FROM grants WHERE agent_name='A';", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT group_concat(value) = 'memory_note_add'"
        " FROM grants WHERE agent_name='B';", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT value = '[\"file_read\",\"memory_block_create\","
        "\"memory_block_update\",\"memory_note_add\",\"memory_note_edit\","
        "\"memory_note_delete\"]' FROM config WHERE key='agent_default_tools';", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM tools WHERE name IN"
        " ('memory_create','memory_add','memory_edit','memory_delete');", 0, -1) == 0);

    db_close(db);
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_v57\n");
}

/* v58 -> v59 in isolation: a v58-shaped database holding representative rows
 * and one orphan of every kind the patch has a rule for. Afterwards every
 * existing routing list is a chat list, memory_blocks says created_by, the
 * configuration-side links are declared and hold (foreign_key_check is
 * empty), the dead columns are gone, and each orphan went the way the patch
 * comment says — deleted when it was only a dangling pointer, NULLed when the
 * row it sat on is worth keeping. */
static void test_schema_patch_v59(void) {
    const char *path = "/tmp/test_cclaw_schema_v59.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v58.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v59(db);
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "PRAGMA foreign_keys=OFF;"
        "INSERT INTO agents(name) VALUES('Scout'),('Writer');"
        "INSERT INTO providers(name, base_url, status) VALUES('local','http://l','healthy');"
        "INSERT INTO models(id, provider_name, model, capabilities) VALUES"
        " ('chat@local','local','chat','[]'),"
        " ('ears@local','local','ears','[\"audio\"]'),"
        " ('lost@gone','gone','lost','[]');"
        "INSERT INTO agent_models(agent_name, model_id, pos, reasoning_effort) VALUES"
        " ('Scout','chat@local',0,'high'),('Scout','lost@gone',1,NULL),"
        " ('Writer','ears@local',0,NULL);"
        "INSERT INTO memory_blocks(id, agent_name, label) VALUES"
        " (5,'Scout','notes'),(6,NULL,'team');"
        "INSERT INTO agent_memory_blocks(agent_name, block_id, read_only) VALUES"
        " ('Scout',5,0),('Writer',5,1),('Writer',6,0);"
        "INSERT INTO extensions(name, path, owner_agent, published) VALUES"
        " ('builtin','/b','system',1),('mine','/m','Scout',0),('stale','/s','OldName',1);"
        "INSERT INTO tools(name, extension_name, path) VALUES"
        " ('builtin_c',NULL,NULL),('blank_ext','',NULL),('mine_t','mine','/m/t'),"
        " ('ghost_t','ghost','/g/t');"
        "INSERT INTO hooks(extension_name, event, path) VALUES"
        " ('mine','preAdvance','/m/h'),('ghost','preAdvance','/g/h');"
        "INSERT INTO agent_extensions(agent_name, extension_name) VALUES"
        " ('Scout','mine'),('Writer','ghost');"
        "INSERT INTO channels(name, extension_name, prev_extension_name, binary_path,"
        "                     status, default_agent) VALUES"
        " ('real','mine',NULL,'/m/c','active','Scout'),"
        " ('blank','',NULL,'','draft',NULL),"
        " ('dangling','ghost','ghost2','','draft','Nobody');"
        "INSERT INTO channel_state(channel_name, key, value) VALUES"
        " ('real','offset','1'),('nochannel','offset','2');"
        "INSERT INTO secrets(name, value) VALUES('KEPT','enc:00');"
        "INSERT INTO secret_hosts(secret_name, host) VALUES"
        " ('KEPT','api.example.com'),('ENV_ONLY','env.example.com');"
        "INSERT INTO approvals(id, session_id, state) VALUES(7,1,'pending');"
        "INSERT INTO approval_decisions(approval_id, decision, decided_via) VALUES"
        " (7,'once','dashboard'),(99,'deny','dashboard');"
        "INSERT INTO cron_jobs(agent_name, name, cron_expr, kind, session_id, task)"
        " VALUES('Scout','daily','0 9 * * *','task',1,'hi');"
        "INSERT INTO llm_jobs(session_id, agent_name, status) VALUES(1,'Scout','pending');"
        "PRAGMA foreign_keys=ON;", NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "v59 seed: %s\n", err ? err : "?");
    assert(rc == SQLITE_OK);
    set_user_version(db, 58);

    assert(db_schema_compat(db) == 1);
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT && uv == CCLAW_SCHEMA_VERSION);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;", 0, -1) == 0);

    /* Decision 11: every existing list is a chat list; effort survives. */
    assert(db_scalar_i64(db, "SELECT group_concat(agent_name||'/'||purpose||'/'||pos||'/'"
        "||model_id||'/'||COALESCE(reasoning_effort,'-'), ' ' ORDER BY agent_name, pos)"
        " = 'Scout/chat/0/chat@local/high Writer/chat/0/ears@local/-'"
        " FROM agent_models;", 0, -1) == 1);
    /* The model on a vanished provider and its routing row are gone. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM models WHERE id='lost@gone';", 0, -1) == 0);

    /* Decision 18: renamed, data and uniqueness intact. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM memory_blocks"
        " WHERE (id=5 AND created_by='Scout') OR (id=6 AND created_by IS NULL);", 0, -1) == 2);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM agent_memory_blocks;", 0, -1) == 3);
    assert(sqlite3_exec(db, "INSERT INTO memory_blocks(created_by, label)"
        " VALUES('Scout','notes');", NULL, NULL, NULL) != SQLITE_OK);

    /* Decision 19, the orphan rules. */
    assert(db_scalar_i64(db, "SELECT group_concat(name||'='||COALESCE(owner_agent,'NULL'),"
        " ' ' ORDER BY name) = 'builtin=NULL mine=Scout stale=NULL' FROM extensions;",
        0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT group_concat(name||'='||COALESCE(extension_name,'NULL'),"
        " ' ' ORDER BY name) = 'blank_ext=NULL builtin_c=NULL mine_t=mine' FROM tools;",
        0, -1) == 1);
    /* v59 kept 'mine' and dropped 'ghost'; v62, replayed after it, empties
     * the table (those events no longer exist). */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM hooks;", 0, -1) == 0);
    assert(db_scalar_i64(db, "SELECT group_concat(agent_name||'>'||extension_name) ="
        " 'Scout>mine' FROM agent_extensions;", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT group_concat(name||':'||COALESCE(extension_name,'NULL')"
        "||':'||COALESCE(prev_extension_name,'NULL')||':'||COALESCE(default_agent,'NULL')"
        "||':'||status, ' ' ORDER BY name) ="
        " 'blank:NULL:NULL:NULL:draft dangling:NULL:NULL:NULL:draft"
        " real:mine:NULL:Scout:active' FROM channels;", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT group_concat(channel_name) = 'real'"
        " FROM channel_state;", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT group_concat(approval_id) = '7'"
        " FROM approval_decisions;", 0, -1) == 1);
    /* No FK on secret_hosts: an environment secret's binding has no secrets
     * row to point at, and must survive. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM secret_hosts;", 0, -1) == 2);

    /* The declared links now refuse a dangling write... */
    assert(sqlite3_exec(db, "INSERT INTO tools(name, extension_name)"
        " VALUES('x','nope');", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "INSERT INTO models(id, provider_name, model)"
        " VALUES('x@nope','nope','x');", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "INSERT INTO approval_decisions(approval_id, decision,"
        " decided_via) VALUES(1234,'deny','t');", NULL, NULL, NULL) != SQLITE_OK);
    /* ...and an agent rename now reaches extension ownership. */
    assert(agent_rename(db, "Scout", "Ranger", 0) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM extensions WHERE owner_agent='Ranger';",
        0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM memory_blocks WHERE created_by='Ranger';",
        0, -1) == 1);

    /* Decision 20: the dead columns are gone; models keeps its health. */
    assert(db_scalar_i64(db,
        "SELECT (SELECT COUNT(*) FROM pragma_table_info('providers') WHERE name='status')"
        "     + (SELECT COUNT(*) FROM pragma_table_info('llm_jobs') WHERE name='status')"
        "     + (SELECT COUNT(*) FROM pragma_table_info('cron_jobs') WHERE name='kind')"
        "     + (SELECT COUNT(*) FROM pragma_table_info('channels') WHERE name='binary_path')"
        "     + (SELECT COUNT(*) FROM sqlite_master WHERE name='idx_llm_jobs_pending');",
        0, -1) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM pragma_table_info('models')"
        " WHERE name IN ('status','degraded_until');", 0, -1) == 2);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM llm_jobs;", 0, -1) == 1);

    /* Decision 12 holds on the upgraded database too (the trigger came with
     * the patch, not only with schema.sql). */
    assert(sqlite3_exec(db, "INSERT INTO agent_models(agent_name, purpose, model_id, pos)"
        " VALUES('Writer','audio','chat@local',0);", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "INSERT INTO agent_models(agent_name, purpose, model_id, pos)"
        " VALUES('Writer','audio','ears@local',0);", NULL, NULL, NULL) == SQLITE_OK);

    db_close(db);
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_v59\n");
}

/* Decision 12: a media purpose needs the capability, on every write path —
 * insert, update of purpose, update of model — while chat rows never ask. */
/* v59 -> v60 in isolation (plan v0.8 decisions 13–17): every existing grant
 * becomes 'legacy' with no approval; every agent may still use every secret
 * it could reach before — stored agent-scope secrets, environment secrets the
 * database knows only through a binding, and the migrating process's own
 * CCLAW_SECRET_* names — and gets nothing for a system secret; the migration
 * is the first thing the new change log records, labelled schema:v60. */
static void test_schema_patch_v60(void) {
    const char *path = "/tmp/test_cclaw_schema_v60.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v59.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v60(db);
    assert(sqlite3_exec(db,
        "INSERT INTO agents(name, created_by) VALUES('Boss',NULL),('Kid','Boss');"
        "INSERT INTO approvals(id, session_id, tool_name, state) VALUES"
        " (7,1,'request_config','approved');"
        "INSERT INTO grants(agent_name, kind, value, approval_mode, expires_at) VALUES"
        " ('Boss','tool','shell_exec','always',NULL),('Boss','host','.ex.com','silent',NULL),"
        " ('Boss','read_path','/srv','silent',NULL),('Kid','write_path','/tmp/k','silent',"
        "  unixepoch()+60);"
        "INSERT INTO secrets(name, value, source, scope) VALUES"
        " ('STORED','enc:00','operator','agent'),('PROVIDER_KEY','enc:01','operator','system');"
        "INSERT INTO secret_hosts(secret_name, host) VALUES"
        " ('STORED','a.ex.com'),('ENV_BOUND','b.ex.com'),('ENV_BOUND','c.ex.com'),"
        " ('PROVIDER_KEY','p.ex.com');",
        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 59);
    setenv("CCLAW_SECRET_ENV_UNBOUND", "env-value-12345", 1);
    setenv("CCLAW_SECRET_PROVIDER_KEY", "shadowed-system", 1);
    db_set_actor(db, "daemon");

    assert(db_schema_compat(db) == 1);
    unsetenv("CCLAW_SECRET_ENV_UNBOUND");
    unsetenv("CCLAW_SECRET_PROVIDER_KEY");
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT && uv == CCLAW_SCHEMA_VERSION);

    /* Existing rows: untouched apart from provenance, which is unknown. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE kind!='secret'", 0, -1) == 4);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE kind!='secret'"
        " AND source='legacy' AND approval_id IS NULL", 0, -1) == 4);
    assert(db_scalar_i64(db, "SELECT approval_mode='always' FROM grants"
        " WHERE agent_name='Boss' AND value='shell_exec'", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT expires_at > unixepoch() FROM grants"
        " WHERE agent_name='Kid' AND kind='write_path'", 0, -1) == 1);
    /* Secret use: three names x two agents, all 'migration'; none for the
     * system secret (whether named by a row, a binding or the environment). */
    assert(db_scalar_i64(db, "SELECT group_concat(agent_name||':'||value, ',')"
        " = 'Boss:ENV_BOUND,Boss:ENV_UNBOUND,Boss:STORED,Kid:ENV_BOUND,Kid:ENV_UNBOUND,Kid:STORED'"
        " FROM (SELECT agent_name, value FROM grants WHERE kind='secret'"
        "       ORDER BY agent_name, value)", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE kind='secret'"
        " AND (source!='migration' OR approval_id IS NOT NULL)", 0, -1) == 0);
    /* The log opens with the migration, attributed and labelled. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM change_log", 0, -1) == 6);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM change_log WHERE actor='daemon'"
        " AND context='schema:v60' AND table_name='grants' AND action='insert'", 0, -1) == 6);
    /* Shape: the constraint is real, links hold, nothing dangles. */
    assert(sqlite3_exec(db, "INSERT INTO grants(agent_name,kind,value)"
        " VALUES('Boss','tool','x')", NULL, NULL, NULL) != SQLITE_OK);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM pragma_foreign_key_list('grants')"
        " WHERE \"table\"='approvals' AND \"from\"='approval_id'", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM pragma_foreign_key_check", 0, -1) == 0);
    /* An approval id written later must point at a real approval. */
    assert(agent_config_grant(db, "Kid", "tool", "y", 0,
                              GRANT_ORIGIN("approval", 7)) == 0);
    assert(sqlite3_exec(db, "INSERT INTO grants(agent_name,kind,value,source,approval_id)"
        " VALUES('Kid','tool','z','approval',99)", NULL, NULL, NULL) != SQLITE_OK);

    db_close(db);
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_v60\n");
}

/* v60 -> v61 in isolation (plan v0.8 decisions 1–10): every channel+chat any
 * route, session or cron job names becomes a channel_chats row with its
 * values from the right place; a session two chats pinned keeps the chat it
 * names and the other gets a fresh session for the same agent; stamps on a
 * channel with no row are dropped (the cron job disabled); channel edges keep
 * their cursor and lose their policy; no id is reused; and the new links
 * hold and refuse a dangling write. */
static void test_schema_patch_v61(void) {
    const char *path = "/tmp/test_cclaw_schema_v61.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v60.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v61(db);
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "INSERT INTO agents(name) VALUES('Main'),('Helper');"
        "INSERT INTO channels(name, status, default_agent, default_tool_filter) VALUES"
        " ('tg','active',NULL,NULL),('dc','active','Helper','[\"web_fetch\"]');"
        "INSERT INTO sessions(id, name, agent_name, channel_name, chat_id, chat_title,"
        "  chat_is_admin, chat_error_notice_at, parent_session_id, tool_filter,"
        "  created_at, updated_at) VALUES"
        " (1,'tg','Main','tg','111','DM with alice',1,NULL,-1,NULL,1000,9000),"
        " (3,'tg','Main','tg','-100','Old title',0,7000,-1,NULL,1100,2000),"
        " (2,'tg','Main','tg','-100','Ops',0,5000,-1,'[\"a\",\"b\"]',2100,8000),"
        " (4,'dc','Helper','dc','555','#general',0,NULL,-1,'[\"web_fetch\"]',1200,7000),"
        " (5,'cron','Helper','dc','555',NULL,NULL,NULL,-1,NULL,1300,6000),"
        " (6,'agent','Main',NULL,NULL,NULL,NULL,NULL,1,'[\"file_read\"]',1400,5000),"
        " (8,'dc','Helper','dc','888','#eight',NULL,NULL,-1,NULL,1600,3500),"
        " (10,'tg','Main','tg','333','Gone',0,1234,-1,NULL,1800,2500),"
        " (12,'gone','Main','gone','x1',NULL,NULL,NULL,-1,NULL,1950,2300),"
        " (13,'tg','Main','tg','445',NULL,NULL,NULL,-1,NULL,1960,2200);"
        "INSERT INTO channel_routes VALUES"
        " ('tg','111',1,'auto',NULL,NULL),"
        " ('tg','-100',2,'explicit','[\"a\",\"b\"]','be brief'),"
        " ('dc','555',4,'auto',NULL,NULL),"
        " ('dc','888',8,'auto',NULL,NULL),"
        " ('dc','999',8,'explicit','[\"web_fetch\"]',NULL),"
        " ('tg','444',13,'auto',NULL,NULL),"
        " ('gone','x2',6,'auto',NULL,NULL);"
        "INSERT INTO delivery_edges(session_id, target_kind, target_ref, policy, cursor)"
        " VALUES (1,'channel','tg','quiescent',10),(4,'channel','dc','turn',20),"
        "        (6,'parent','1','digest',3),(12,'channel','gone','turn',0);"
        "INSERT INTO cron_jobs(id, agent_name, name, cron_expr, session_id, task,"
        "  channel_name, chat_id, target) VALUES"
        " (5,'Main','follow','0 8 * * *',1,'ping','tg','111',NULL),"
        " (6,'Helper','report','0 7 * * *',5,'r','dc','555','new'),"
        " (7,'Main','orphan','0 6 * * *',0,'x','tg','446',NULL),"
        " (8,'Main','lost','0 6 * * *',0,'x','gone','x3',NULL);"
        "INSERT INTO sessions(id, name, agent_name) VALUES(40,'tmp','Main');"
        "DELETE FROM sessions WHERE id=40;",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "v61 seed: %s\n", err ? err : "?");
    assert(rc == SQLITE_OK);
    set_user_version(db, 60);

    assert(db_schema_compat(db) == 1);
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT && uv == CCLAW_SCHEMA_VERSION);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM pragma_foreign_key_check;", 0, -1) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sqlite_master"
        " WHERE name='channel_routes';", 0, -1) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM pragma_table_info('sessions')"
        " WHERE name IN ('chat_title','chat_is_admin','chat_error_notice_at');", 0, -1) == 0);

    /* One row per chat anything named: routes (minus the dead channel's),
     * the chat a session alone names (333), the one a cron job alone names
     * (446). 445 is gone: its only session was re-stamped to its pin. */
    assert(db_scalar_i64(db, "SELECT group_concat(channel_name||':'||chat_id, ' ')"
        " = 'dc:555 dc:888 dc:999 tg:-100 tg:111 tg:333 tg:444 tg:446'"
        " FROM (SELECT * FROM channel_chats ORDER BY channel_name, chat_id);", 0, -1) == 1);
    /* Values: route settings; title/admin from the newest session with one;
     * error notice = MAX over the chat's sessions. */
    assert(db_scalar_i64(db, "SELECT session_id=2 AND delivery_mode='explicit'"
        " AND tool_filter='[\"a\",\"b\"]' AND guidance='be brief' AND title='Ops'"
        " AND is_admin=0 AND error_notice_at=7000 AND kind IS NULL"
        " AND first_seen_at=1100 AND last_message_at IS NULL AND notified_at IS NULL"
        " FROM channel_chats WHERE chat_id='-100';", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT session_id=1 AND delivery_mode='auto'"
        " AND tool_filter IS NULL AND title='DM with alice' AND is_admin=1"
        " FROM channel_chats WHERE chat_id='111';", 0, -1) == 1);
    /* A route with no filter whose session froze one keeps it on the chat. */
    assert(db_scalar_i64(db, "SELECT session_id=4 AND tool_filter='[\"web_fetch\"]'"
        " FROM channel_chats WHERE chat_id='555';", 0, -1) == 1);
    /* Unrouted: defaults (tg has no default filter), traffic facts kept. */
    assert(db_scalar_i64(db, "SELECT session_id IS NULL AND delivery_mode='auto'"
        " AND tool_filter IS NULL AND title='Gone' AND error_notice_at=1234"
        " FROM channel_chats WHERE chat_id='333';", 0, -1) == 1);
    /* Two chats, one session: 888 (the chat session 8 names) keeps it; 999
     * gets a fresh Helper session above the old high-water mark. */
    assert(db_scalar_i64(db, "SELECT session_id FROM channel_chats WHERE chat_id='888';",
        0, -1) == 8);
    assert(db_scalar_i64(db, "SELECT c.session_id=41 AND s.agent_name='Helper'"
        " AND s.channel_name='dc' AND s.chat_id='999' AND c.delivery_mode='explicit'"
        " FROM channel_chats c JOIN sessions s ON s.id=c.session_id"
        " WHERE c.chat_id='999';", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT seq FROM sqlite_sequence WHERE name='sessions';",
        0, -1) == 41);
    /* Re-stamp; dropped stamps; the disabled job; the old session still
     * names its chat. */
    assert(db_scalar_i64(db, "SELECT chat_id='444' FROM sessions WHERE id=13;", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT channel_name IS NULL AND chat_id IS NULL"
        " FROM sessions WHERE id=12;", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT channel_name IS NULL AND enabled=0"
        " FROM cron_jobs WHERE id=8;", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT channel_name='tg' AND chat_id='-100'"
        " FROM sessions WHERE id=3;", 0, -1) == 1);
    /* Channel edges: cursor kept, policy gone; the dead channel's edge gone;
     * the parent edge untouched. */
    assert(db_scalar_i64(db, "SELECT group_concat(session_id||':'||target_kind||':'"
        "||COALESCE(policy,'-')||':'||cursor, ' ')"
        " = '1:channel:-:10 4:channel:-:20 6:parent:digest:3'"
        " FROM (SELECT * FROM delivery_edges ORDER BY session_id);", 0, -1) == 1);

    /* The links refuse: a session or job naming no chat row, a second pin of
     * one session, a channel edge with a policy. */
    assert(sqlite3_exec(db, "INSERT INTO sessions(agent_name, channel_name, chat_id)"
        " VALUES('Main','tg','nope');", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "INSERT INTO cron_jobs(agent_name, name, cron_expr,"
        " session_id, task, channel_name, chat_id) VALUES('Main','n','',0,'',"
        "'tg','nope');", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE channel_chats SET session_id=1"
        " WHERE chat_id='333';", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "INSERT INTO delivery_edges(session_id, target_kind,"
        " target_ref, policy) VALUES(2,'channel','tg','turn');", NULL, NULL, NULL)
        != SQLITE_OK);
    assert(sqlite3_exec(db, "INSERT INTO channel_chats(channel_name, chat_id)"
        " VALUES('nochannel','1');", NULL, NULL, NULL) != SQLITE_OK);

    db_close(db);
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_v61\n");
}

/* v61 cursors: an edge that never posted must not post an old answer after
 * the upgrade, whichever way its chat reads now. Rule: a channel edge frozen
 * 'explicit' under v60, or whose chat is 'explicit' now, gets the cursor an
 * explicit boundary stamps today (sessions.leaf_id); any other edge keeps its
 * cursor (behind = owed under v60 too). Cases, each with an assistant leaf:
 *   1 routed explicit chat, edge frozen explicit, cursor 0     -> leaf
 *   2 routed auto chat,     edge frozen explicit, cursor 0     -> leaf (mirror)
 *   3 unrouted group-shaped chat (patch makes it explicit), edge 'turn', 0 -> leaf
 *   4 unrouted chat (patch makes it auto), edge frozen explicit, 0 -> leaf
 *   5 older non-pinned session of chat 1, edge frozen explicit -> leaf
 *   6 older non-pinned session of chat 2, edge 'turn', delivered -> unchanged
 *   7 routed auto chat, edge quiescent, cursor behind (owed)   -> unchanged
 * Then the sweep posts only 7's owed answer, and flipping chat 1 to auto
 * posts nothing old. */
static void test_schema_patch_v61_cursors(void) {
    const char *path = "/tmp/test_cclaw_schema_v61_cursors.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v60.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v61(db);
    char *err = NULL;
    int rc = sqlite3_exec(db,
        "INSERT INTO agents(name) VALUES('Main');"
        "INSERT INTO channels(name, status) VALUES('tg','draft');"
        "INSERT INTO sessions(id, name, agent_name, channel_name, chat_id) VALUES"
        " (1,'a','Main','tg','-100'),(2,'b','Main','tg','200'),(3,'c','Main','tg','-300'),"
        " (4,'d','Main','tg','400'),(5,'e','Main','tg','-100'),(6,'f','Main','tg','200'),"
        " (7,'g','Main','tg','700');"
        "INSERT INTO entries(id, session_id, role, content) VALUES"
        " (10,1,1,'q'),(11,1,2,'a1 kept'),(20,2,1,'q'),(21,2,2,'a2 kept'),"
        " (30,3,2,'a3 kept'),(40,4,2,'a4 kept'),(50,5,2,'a5 kept'),"
        " (60,6,2,'a6 sent'),(70,7,1,'q'),(71,7,2,'a7 owed');"
        "INSERT INTO channel_routes VALUES"
        " ('tg','-100',1,'explicit',NULL,NULL),('tg','200',2,'auto',NULL,NULL),"
        " ('tg','700',7,'auto',NULL,NULL);"
        "INSERT INTO delivery_edges(session_id, target_kind, target_ref, policy, cursor) VALUES"
        " (1,'channel','tg','explicit',0),(2,'channel','tg','explicit',0),"
        " (3,'channel','tg','turn',0),(4,'channel','tg','explicit',0),"
        " (5,'channel','tg','explicit',0),(6,'channel','tg','turn',60),"
        " (7,'channel','tg','quiescent',70);",
        NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "v61 cursors seed: %s\n", err ? err : "?");
    assert(rc == SQLITE_OK);
    set_user_version(db, 60);
    assert(db_schema_compat(db) == 1);

    assert(db_scalar_i64(db, "SELECT group_concat(session_id||':'||cursor, ' ')"
        " = '1:11 2:21 3:30 4:40 5:50 6:60 7:70'"
        " FROM (SELECT * FROM delivery_edges ORDER BY session_id);", 0, -1) == 1);
    /* The mode each chat reads now: as the comment says. */
    assert(db_scalar_i64(db, "SELECT group_concat(chat_id||'='||delivery_mode, ' ')"
        " = '-100=explicit -300=explicit 200=auto 400=auto 700=auto'"
        " FROM (SELECT * FROM channel_chats ORDER BY chat_id);", 0, -1) == 1);

    /* The first sweep after the upgrade: only the delivery v60 already owed. */
    assert(advance_sweep_undelivered(db) == 1);
    assert(db_scalar_i64(db, "SELECT group_concat(json_extract(payload,'$.text'))"
        " = 'a7 owed' FROM channel_outbox;", 0, -1) == 1);
    /* Flip the explicit chats to auto: nothing old posts. */
    assert(sqlite3_exec(db, "UPDATE channel_chats SET delivery_mode='auto';",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(advance_sweep_undelivered(db) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM channel_outbox;", 0, -1) == 1);

    db_close(db);
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_v61_cursors\n");
}

/* A v60 database holding a row that already dangles (a session of an agent
 * that does not exist, written with foreign keys off) is refused by v61 and
 * left exactly as it was — failure degrades to nothing happened. */
static void test_schema_patch_v61_refuses_dangling(void) {
    const char *path = "/tmp/test_cclaw_schema_v61_dangling.sqlite";
    char junk[192];
    const char *suffixes[] = { "", "-wal", "-shm", ".v60.bak" };
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v61(db);
    assert(sqlite3_exec(db, "PRAGMA foreign_keys=OFF;"
        "INSERT INTO sessions(id, name, agent_name) VALUES(5,'x','Ghost');"
        "PRAGMA foreign_keys=ON;", NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 60);
    assert(db_schema_compat(db) == 0);
    int uv = 0;
    db_schema_state(db, &uv);
    assert(uv == 60);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sqlite_master"
        " WHERE name IN ('channel_routes')", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sqlite_master"
        " WHERE name IN ('channel_chats')", 0, -1) == 0);
    db_close(db);
    for (size_t i = 0; i < 4; i++) {
        snprintf(junk, sizeof(junk), "%s%s", path, suffixes[i]); unlink(junk);
    }
    printf("  PASS test_schema_patch_v61_refuses_dangling\n");
}

/* v62: in-process hooks are gone — hook_directives is dropped (with its
 * index) and every hooks row is deleted; the hooks table and the extension
 * the rows belonged to stay. */
static void test_schema_patch_v62(void) {
    const char *path = "/tmp/test_cclaw_schema_v62.sqlite";
    test_db_clean(path);
    sqlite3 *db = db_open(path);
    assert(db != NULL && db_ensure_schema(db) == 0);
    unshape_v62(db);
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name, path, published) VALUES('ext','/e',1);"
        "INSERT INTO hooks(extension_name, event, path) VALUES"
        " ('ext','preAdvance','/e/a.qjs'),('ext','beforeToolCall','/e/g.qjs');"
        "INSERT INTO hook_directives(session_id, kind, role, content)"
        " VALUES(1,'inject','system','stale');",
        NULL, NULL, NULL) == SQLITE_OK);
    set_user_version(db, 61);

    assert(db_schema_compat(db) == 1);
    int uv = 0;
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT && uv == CCLAW_SCHEMA_VERSION);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sqlite_master"
        " WHERE name IN ('hook_directives','idx_hook_directives_session');", 0, -1) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sqlite_master"
        " WHERE type='table' AND name='hooks';", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM hooks;", 0, -1) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM extensions WHERE name='ext';",
        0, -1) == 1);

    /* A v61 database that already lacks the table still upgrades. */
    set_user_version(db, 61);
    assert(db_schema_compat(db) == 1);
    assert(db_schema_state(db, &uv) == DB_SCHEMA_CURRENT && uv == CCLAW_SCHEMA_VERSION);

    db_close(db);
    test_db_clean(path);
    printf("  PASS test_schema_patch_v62\n");
}

static void test_agent_models_capability_gate(void) {
    const char *path = "/tmp/test_cclaw_db_capgate.sqlite";
    test_db_clean(path);
    sqlite3 *db = test_db_open(path);
    assert(db != NULL);
    assert(sqlite3_exec(db,
        "INSERT INTO agents(name) VALUES('A');"
        "INSERT INTO providers(name, base_url) VALUES('p','http://p');"
        "INSERT INTO models(id, provider_name, model, capabilities) VALUES"
        " ('plain@p','p','plain','[]'), ('eyes@p','p','eyes','[\"image\",\"chat\"]'),"
        " ('ears@p','p','ears','[\"audio\"]'), ('junk@p','p','junk','not json'),"
        " ('none@p','p','none',NULL);", NULL, NULL, NULL) == SQLITE_OK);
    const char *ins = "INSERT INTO agent_models(agent_name, purpose, model_id, pos)"
                      " VALUES('A', ?1, ?2, ?3)";
    struct { const char *purpose, *model; int pos, ok; } cases[] = {
        { "chat",  "plain@p", 0, 1 },   /* chat never checks capabilities */
        { "image", "plain@p", 0, 0 },
        { "image", "junk@p",  0, 0 },   /* malformed JSON is no capability */
        { "image", "none@p",  0, 0 },
        { "image", "ears@p",  0, 0 },
        { "image", "eyes@p",  0, 1 },
        { "audio", "eyes@p",  0, 0 },
        { "audio", "ears@p",  0, 1 },
        { "video", "eyes@p",  1, 0 },   /* not a purpose (CHECK) */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); i++) {
        sqlite3_stmt *st;
        assert(sqlite3_prepare_v2(db, ins, -1, &st, NULL) == SQLITE_OK);
        sqlite3_bind_text(st, 1, cases[i].purpose, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, cases[i].model, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 3, cases[i].pos);
        int r = sqlite3_step(st);
        if ((r == SQLITE_DONE) != cases[i].ok)
            fprintf(stderr, "capgate case %zu (%s %s): rc=%d %s\n", i, cases[i].purpose,
                    cases[i].model, r, sqlite3_errmsg(db));
        assert((r == SQLITE_DONE) == cases[i].ok);
        if (!cases[i].ok && r == SQLITE_CONSTRAINT &&
            strcmp(cases[i].purpose, "video") != 0)
            assert(strstr(sqlite3_errmsg(db), "capabilities do not include"));
        sqlite3_finalize(st);
    }
    /* Updates cannot sneak around it either. */
    assert(sqlite3_exec(db, "UPDATE agent_models SET model_id='plain@p'"
        " WHERE purpose='audio';", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE agent_models SET purpose='audio'"
        " WHERE purpose='chat';", NULL, NULL, NULL) != SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE agent_models SET pos=3"
        " WHERE purpose='audio';", NULL, NULL, NULL) == SQLITE_OK);
    /* One list per purpose: the same pos in two purposes is two rows. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM agent_models WHERE agent_name='A';",
        0, -1) == 3);
    db_close(db);
    test_db_clean(path);
    printf("  PASS test_agent_models_capability_gate\n");
}

int main(void) {
    TEST_INIT();
    printf("test_db:\n");
    test_wal_mode();
    test_reopen_idempotent();
    test_fts5_search();
    test_config_registry();
    test_agent_pragmas();
    test_prune_inbox();
    test_prune_outbox();
    test_free_mb();
    test_seed_backfills_and_preserves();
    test_schema_state();
    test_schema_patch_application();
    test_schema_patch_v47();
    test_schema_patch_v57();
    test_schema_patch_v48();
    test_schema_patch_v50();
    test_schema_patch_v51();
    test_schema_patch_v59();
    test_schema_patch_v60();
    test_schema_patch_v61();
    test_schema_patch_v61_refuses_dangling();
    test_schema_patch_v61_cursors();
    test_schema_patch_v62();
    test_agent_models_capability_gate();
    test_rate_limit_and_cost();
    printf("All db tests passed.\n");
    return 0;
}
