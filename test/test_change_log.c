/* The change log (schema v60, plan v0.8 decisions 14/15): triggers generated
 * from one setting/measurement list write one row per changed configuration
 * row, naming the handle that did it. These tests pin the list against the
 * schema (nothing unclassified, nothing stale, triggers == list), the
 * "settings only" rule (a counter update writes nothing), the no-op rules,
 * the actor/context plumbing, and the one guarantee that matters most: a
 * secret's value never reaches the log in any form. */
#define _GNU_SOURCE
#include "agent_config.h"
#include "change_log.h"
#include "db.h"
#include "secret.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DB_PATH "/tmp/cclaw_test_change_log.db"

static sqlite3 *fresh(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    return db;
}

static void exec_ok(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "exec failed: %s\n  %s\n", err ? err : "?", sql);
        assert(0);
    }
}

static int64_t log_rows(sqlite3 *db) {
    return db_scalar_i64(db, "SELECT COUNT(*) FROM change_log", 0, -1);
}

/* Every column of every logged table is classified, and every classified
 * column exists — the same shape as test_fk_shape_audit: a new column on a
 * logged table fails here until someone decides whether it is a setting. */
static void test_every_column_classified(void) {
    sqlite3 *db = fresh();
    int bad = 0;
    /* Distinct listed tables. */
    for (size_t i = 0; i < CHANGE_LOG_COLUMN_COUNT; i++) {
        const char *t = CHANGE_LOG_COLUMNS[i].table;
        int seen = 0;
        for (size_t j = 0; j < i; j++)
            if (strcmp(CHANGE_LOG_COLUMNS[j].table, t) == 0) seen = 1;
        if (seen) continue;
        sqlite3_stmt *st;
        assert(sqlite3_prepare_v2(db, "SELECT name FROM pragma_table_info(?1)",
                                  -1, &st, NULL) == SQLITE_OK);
        sqlite3_bind_text(st, 1, t, -1, SQLITE_STATIC);
        int ncols = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *c = (const char *)sqlite3_column_text(st, 0);
            ncols++;
            if (change_log_class(t, c) < 0) {
                printf("  UNCLASSIFIED: %s.%s — add it to CHANGE_LOG_COLUMNS\n", t, c);
                bad++;
            }
        }
        sqlite3_finalize(st);
        if (ncols == 0) { printf("  listed table %s does not exist\n", t); bad++; }
    }
    /* Stale entries: listed column no longer in its table. */
    for (size_t i = 0; i < CHANGE_LOG_COLUMN_COUNT; i++) {
        char sql[192];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM pragma_table_info('%s') WHERE name='%s'",
                 CHANGE_LOG_COLUMNS[i].table, CHANGE_LOG_COLUMNS[i].column);
        if (db_scalar_i64(db, sql, 0, 0) != 1) {
            printf("  STALE: %s.%s is listed but not in the schema\n",
                   CHANGE_LOG_COLUMNS[i].table, CHANGE_LOG_COLUMNS[i].column);
            bad++;
        }
    }
    assert(bad == 0);
    db_close(db);
    printf("  PASS test_every_column_classified\n");
}

/* The triggers a fresh database carries are exactly the generated ones: three
 * per listed table, text identical to change_log_trigger_sql, and no other
 * log_* trigger. */
static void test_triggers_match_list(void) {
    sqlite3 *db = fresh();
    static const char *ACTS[] = { "ins", "upd", "del" };
    int expected = 0;
    for (size_t i = 0; i < CHANGE_LOG_COLUMN_COUNT; i++) {
        const char *t = CHANGE_LOG_COLUMNS[i].table;
        int seen = 0;
        for (size_t j = 0; j < i; j++)
            if (strcmp(CHANGE_LOG_COLUMNS[j].table, t) == 0) seen = 1;
        if (seen) continue;
        for (int a = 0; a < 3; a++) {
            char name[96];
            snprintf(name, sizeof(name), "log_%s_%s", t, ACTS[a]);
            char *want = change_log_trigger_sql(db, t, ACTS[a]);
            assert(want);
            sqlite3_stmt *st;
            assert(sqlite3_prepare_v2(db, "SELECT sql FROM sqlite_master"
                " WHERE type='trigger' AND name=?1 AND tbl_name=?2",
                -1, &st, NULL) == SQLITE_OK);
            sqlite3_bind_text(st, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, t, -1, SQLITE_STATIC);
            assert(sqlite3_step(st) == SQLITE_ROW);
            assert(strcmp((const char *)sqlite3_column_text(st, 0), want) == 0);
            sqlite3_finalize(st);
            free(want);
            expected++;
        }
    }
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger'"
                         " AND name LIKE 'log\\_%' ESCAPE '\\'", 0, -1) == expected);
    /* Sync is idempotent: nothing to do on a matching database. */
    int64_t cookie = db_scalar_i64(db, "PRAGMA schema_version", 0, -1);
    assert(change_log_sync(db) == 0);
    assert(db_scalar_i64(db, "PRAGMA schema_version", 0, -1) == cookie);
    /* And it repairs: a dropped trigger comes back on the next sync. */
    exec_ok(db, "DROP TRIGGER log_grants_upd");
    assert(change_log_sync(db) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM sqlite_master"
                         " WHERE name='log_grants_upd'", 0, -1) == 1);
    db_close(db);
    printf("  PASS test_triggers_match_list\n");
}

/* A grant written without saying where it came from is refused: grants.source
 * has no default on purpose. */
static void test_grant_without_source_refused(void) {
    sqlite3 *db = fresh();
    test_seed_agent(db, "Ann");
    assert(sqlite3_exec(db, "INSERT INTO grants(agent_name,kind,value)"
                        " VALUES('Ann','tool','x')", NULL, NULL, NULL) != SQLITE_OK);
    assert(strstr(sqlite3_errmsg(db), "NOT NULL constraint failed: grants.source"));
    assert(sqlite3_exec(db, "INSERT INTO grants(agent_name,kind,value,source)"
                        " VALUES('Ann','tool','x','because')", NULL, NULL, NULL) != SQLITE_OK);
    assert(strstr(sqlite3_errmsg(db), "CHECK constraint failed"));
    /* The API refuses a missing origin before SQLite has to. */
    assert(agent_config_grant(db, "Ann", "tool", "x", 0, GRANT_ORIGIN(NULL, 0)) == -1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants", 0, -1) == 0);
    db_close(db);
    printf("  PASS test_grant_without_source_refused\n");
}

/* Insert / update / delete each log one row with the actor and context of
 * the handle; a no-op UPDATE, an ignored INSERT OR IGNORE and a no-change
 * upsert log nothing; an upsert that changes something logs an update. */
static void test_actions_and_noops(void) {
    sqlite3 *db = fresh();
    db_set_actor(db, "cli");
    test_seed_agent(db, "Ann");                       /* agents insert: 1 row */
    assert(log_rows(db) == 1);
    db_set_context(db, "approval:41");
    assert(agent_config_grant(db, "Ann", "host", "a.example", 0,
                              GRANT_ORIGIN("approval", 0)) == 0);
    db_set_context(db, NULL);
    assert(db_scalar_i64(db, "SELECT actor='cli' AND context='approval:41'"
        " AND table_name='grants' AND action='insert' AND old_values IS NULL"
        " AND json_extract(row_key,'$.value')='a.example'"
        " AND json_extract(new_values,'$.source')='approval'"
        " FROM change_log ORDER BY id DESC LIMIT 1", 0, -1) == 1);
    int64_t n = log_rows(db);

    /* Same value written again: no row. */
    exec_ok(db, "UPDATE grants SET approval_mode='silent' WHERE value='a.example'");
    exec_ok(db, "UPDATE agents SET description=description WHERE name='Ann'");
    assert(log_rows(db) == n);
    /* INSERT OR IGNORE that ignores: no row. */
    exec_ok(db, "INSERT OR IGNORE INTO grants(agent_name,kind,value,source)"
                " VALUES('Ann','host','a.example','operator')");
    assert(log_rows(db) == n);
    /* Re-grant of a permanent row: the upsert's WHERE leaves it alone. */
    assert(agent_config_grant(db, "Ann", "host", "a.example", 0,
                              GRANT_ORIGIN("operator", 0)) == 0);
    assert(log_rows(db) == n);
    /* An upsert that does change the row logs an update with both sides. */
    assert(agent_config_set_tool_mode(db, "Ann", "a_tool", "always",
                                      GRANT_ORIGIN("operator", 0)) == 0);   /* insert */
    assert(agent_config_set_tool_mode(db, "Ann", "a_tool", "silent",
                                      GRANT_ORIGIN("operator", 0)) == 0);   /* update */
    assert(log_rows(db) == n + 2);
    assert(db_scalar_i64(db, "SELECT action='update'"
        " AND json_extract(old_values,'$.approval_mode')='always'"
        " AND json_extract(new_values,'$.approval_mode')='silent'"
        " FROM change_log ORDER BY id DESC LIMIT 1", 0, -1) == 1);
    /* Delete. */
    assert(agent_config_revoke(db, "Ann", "host", "a.example") == 0);
    assert(db_scalar_i64(db, "SELECT action='delete' AND new_values IS NULL"
        " AND json_extract(old_values,'$.value')='a.example'"
        " FROM change_log ORDER BY id DESC LIMIT 1", 0, -1) == 1);
    /* Unlabelled handle: actor 'unknown', never NULL. */
    sqlite3 *other = db_open(DB_PATH);
    exec_ok(other, "UPDATE agents SET description='d' WHERE name='Ann'");
    db_close(other);
    assert(db_scalar_i64(db, "SELECT actor='unknown' AND context IS NULL"
        " FROM change_log ORDER BY id DESC LIMIT 1", 0, -1) == 1);
    db_close(db);
    printf("  PASS test_actions_and_noops\n");
}

/* models is updated on every LLM request; only a setting change may log.
 * The counter update in llm_proc.c's shape fires no trigger at all (UPDATE
 * OF scoping); the success path that rewrites status='healthy' on a healthy
 * row logs nothing (WHEN); a real status flip logs exactly that. */
static void test_measurements_not_logged(void) {
    sqlite3 *db = fresh();
    exec_ok(db, "INSERT INTO providers(name,base_url) VALUES('p','http://p');"
                "INSERT INTO models(id,provider_name,model) VALUES('m@p','p','m');");
    int64_t n = log_rows(db);
    exec_ok(db, "UPDATE models SET total_requests=total_requests+1,"
                " consec_failures=consec_failures+1, last_error_at=unixepoch()"
                " WHERE id='m@p'");
    exec_ok(db, "UPDATE models SET total_requests=total_requests+1,"
                " total_tokens_in=total_tokens_in+5, total_tokens_out=total_tokens_out+7,"
                " total_cost_nano=total_cost_nano+9, last_success_at=unixepoch(),"
                " consec_failures=0, status='healthy', degraded_until=NULL"
                " WHERE id='m@p' AND status != 'disabled'");
    exec_ok(db, "UPDATE agents SET hold_until=unixepoch()+60, hold_holder='cli:1'"
                " WHERE name='nobody'");
    assert(log_rows(db) == n);
    /* The trigger is scoped to the setting columns, so a measurement-only
     * statement never even evaluates it. */
    char *upd = change_log_trigger_sql(db, "models", "upd");
    assert(upd && strstr(upd, "UPDATE OF ") && !strstr(upd, "total_requests")
           && !strstr(upd, "consec_failures") && !strstr(upd, "degraded_until"));
    free(upd);
    exec_ok(db, "UPDATE models SET status='degraded', degraded_until=unixepoch()+300"
                " WHERE id='m@p'");
    assert(log_rows(db) == n + 1);
    assert(db_scalar_i64(db, "SELECT json_extract(old_values,'$.status')='healthy'"
        " AND json_extract(new_values,'$.status')='degraded'"
        " AND json_extract(new_values,'$.degraded_until') IS NULL"
        " AND json_type(new_values,'$.degraded_until') IS NULL"
        " FROM change_log ORDER BY id DESC LIMIT 1", 0, -1) == 1);
    db_close(db);
    printf("  PASS test_measurements_not_logged\n");
}

/* The guarantee: a secret's value — plaintext or the stored ciphertext —
 * appears nowhere in change_log, through set, overwrite and removal. The
 * fact of a change is still recorded. */
static void test_secret_value_never_logged(void) {
    sqlite3 *db = fresh();
    uint8_t key[32];
    assert(secret_key_load_or_create(DB_PATH, key) == 0);
    db_set_secret_key(key);
    const char *v1 = "s3cret-VALUE-one-1234", *v2 = "s3cret-VALUE-two-5678";
    assert(db_secret_set(db, "LOG_TEST", v1, "operator", "agent") == 0);
    char *enc1 = db_scalar_text(db, "SELECT value FROM secrets WHERE name='LOG_TEST'", 0);
    assert(db_secret_set(db, "LOG_TEST", v2, "operator", "agent") == 0);
    char *enc2 = db_scalar_text(db, "SELECT value FROM secrets WHERE name='LOG_TEST'", 0);
    assert(db_secret_rm(db, "LOG_TEST", NULL) == 0);
    assert(enc1 && enc2);

    /* Every column of every row, as text. */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT COALESCE(actor,'')||' '||COALESCE(context,'')"
        "||' '||table_name||' '||row_key||' '||action||' '||COALESCE(old_values,'')"
        "||' '||COALESCE(new_values,'') FROM change_log", -1, &st, NULL) == SQLITE_OK);
    int rows = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *line = (const char *)sqlite3_column_text(st, 0);
        assert(!strstr(line, v1) && !strstr(line, v2));
        assert(!strstr(line, enc1) && !strstr(line, enc2));
        assert(!strstr(line, "enc:"));
        rows++;
    }
    sqlite3_finalize(st);
    assert(rows == 3);   /* insert, update, delete */
    assert(db_scalar_i64(db, "SELECT json_extract(new_values,'$.value')='(set)'"
        " FROM change_log WHERE table_name='secrets' AND action='insert'", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT json_extract(new_values,'$.value')='(changed)'"
        " AND json_type(old_values,'$.value') IS NULL"
        " FROM change_log WHERE table_name='secrets' AND action='update'", 0, -1) == 1);
    free(enc1); free(enc2);
    db_wipe_secret_key();
    db_close(db);
    printf("  PASS test_secret_value_never_logged\n");
}

/* A SQLite connection that does not define cclaw_actor() — a plain system
 * sqlite3 — cannot write a logged table (accepted, decision 14), can still
 * write an unlogged one, and nobody can rewrite history. */
static void test_plain_sqlite_and_append_only(void) {
    sqlite3 *db = fresh();
    test_seed_agent(db, "Ann");
    db_close(db);
    sqlite3 *raw = NULL;
    assert(sqlite3_open(DB_PATH, &raw) == SQLITE_OK);
    assert(sqlite3_exec(raw, "UPDATE agents SET description='x' WHERE name='Ann'",
                        NULL, NULL, NULL) != SQLITE_OK);
    assert(strstr(sqlite3_errmsg(raw), "no such function: cclaw_actor"));
    assert(sqlite3_exec(raw, "INSERT INTO sensitive_targets(kind,value)"
                        " VALUES('host','x.example')", NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(raw);
    db = db_open(DB_PATH);
    assert(sqlite3_exec(db, "DELETE FROM change_log", NULL, NULL, NULL) != SQLITE_OK);
    assert(strstr(sqlite3_errmsg(db), "append-only"));
    assert(sqlite3_exec(db, "UPDATE change_log SET actor='me'", NULL, NULL, NULL) != SQLITE_OK);
    db_close(db);
    printf("  PASS test_plain_sqlite_and_append_only\n");
}

/* Renaming an agent cascades into grants/agent_models; each cascaded row is
 * its own update, keyed by the new name, with the old one in old_values. */
static void test_rename_cascade_logged(void) {
    sqlite3 *db = fresh();
    test_seed_agent(db, "Old");
    assert(agent_config_grant(db, "Old", "tool", "t", 0, GRANT_ORIGIN("operator", 0)) == 0);
    int64_t n = log_rows(db);
    exec_ok(db, "UPDATE agents SET name='New' WHERE name='Old'");
    assert(log_rows(db) == n + 2);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM change_log WHERE table_name='grants'"
        " AND action='update' AND json_extract(row_key,'$.agent_name')='New'"
        " AND json_extract(old_values,'$.agent_name')='Old'", 0, -1) == 1);
    db_close(db);
    printf("  PASS test_rename_cascade_logged\n");
}

int main(void) {
    TEST_INIT();
    printf("test_change_log:\n");
    test_every_column_classified();
    test_triggers_match_list();
    test_grant_without_source_refused();
    test_actions_and_noops();
    test_measurements_not_logged();
    test_secret_value_never_logged();
    test_plain_sqlite_and_append_only();
    test_rename_cascade_logged();
    test_db_clean(DB_PATH);
    printf("All change_log tests passed.\n");
    return 0;
}
