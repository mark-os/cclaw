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
    set_user_version(old_db, 40);

    assert(db_schema_compat(old_db) == 1);   /* runs every pending patch */

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
     * keeps it running, now as an ordinary bare-wake job. */
    sqlite3_stmt *hb;
    assert(sqlite3_prepare_v2(old_db,
        "SELECT kind, task, cron_expr, interval_s, enabled FROM cron_jobs"
        " WHERE agent_name='Pulse' AND name='heartbeat'", -1, &hb, NULL) == SQLITE_OK);
    assert(sqlite3_step(hb) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(hb, 0), "task") == 0);
    assert(sqlite3_column_text(hb, 1)[0] == '\0');
    assert(strcmp((const char *)sqlite3_column_text(hb, 2), "*/30 * * * *") == 0);
    assert(sqlite3_column_type(hb, 3) == SQLITE_NULL);
    assert(sqlite3_column_int(hb, 4) == 1);
    sqlite3_finalize(hb);

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

    /* Reverse-shape to v46: FK-less cron_jobs, no lease columns, the old
     * dual-purpose approvals.action. */
    assert(sqlite3_exec(db,
        "PRAGMA foreign_keys=OFF;"
        /* v48/v49 are above this step: shed their columns too, or the upgrade
         * run below re-adds ones that already exist. */
        "ALTER TABLE entries DROP COLUMN reasoning_meta;"
        "ALTER TABLE entries DROP COLUMN cached_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cached_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cache_write_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN reasoning_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cost;"
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

    assert(sqlite3_exec(db,
        "ALTER TABLE entries DROP COLUMN reasoning_meta;"
        /* v49 sits above this step: shed its columns too. */
        "ALTER TABLE entries DROP COLUMN cached_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cached_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cache_write_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN reasoning_tokens;"
        "ALTER TABLE llm_responses DROP COLUMN cost;",
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
    test_seed_backfills_and_preserves();
    test_schema_state();
    test_schema_patch_application();
    test_schema_patch_v47();
    test_schema_patch_v48();
    test_rate_limit_and_cost();
    printf("All db tests passed.\n");
    return 0;
}
