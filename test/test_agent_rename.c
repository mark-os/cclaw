#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_agent_rename.db"

static sqlite3 *setup(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    /* Seed an agent, plus one it created (created_by is a self-FK and must
     * follow the rename) */
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('OldAgent')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO agents(name,created_by) VALUES('Child','OldAgent')", NULL, NULL, NULL);
    /* Seed related rows */
    sqlite3_exec(db, "INSERT INTO sessions(name,agent_name,state) VALUES('s1','OldAgent','idle')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO agent_extensions(agent_name,extension_name) VALUES('OldAgent','telegram')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO channels(name,default_agent) VALUES('tg','OldAgent')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO cron_jobs(agent_name,name,cron_expr,session_id,task) VALUES('OldAgent','job1','* * * * *',1,'hi')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO memory_blocks(agent_name,label) VALUES('OldAgent','AGENT')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO config(key,value) VALUES('default_agent','OldAgent')", NULL, NULL, NULL);
    return db;
}

static int count_rows(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    int n = 0;
    if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return n;
}

static void test_successful_rename(void) {
    sqlite3 *db = setup();
    int rc = agent_rename(db, "OldAgent", "NewAgent", 0);
    assert(rc == 0);

    /* Verify cascade */
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='OldAgent'") == 0);
    assert(count_rows(db, "SELECT COUNT(*) FROM sessions WHERE agent_name='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM agent_extensions WHERE agent_name='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM channels WHERE default_agent='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM cron_jobs WHERE agent_name='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM memory_blocks WHERE agent_name='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM config WHERE key='default_agent' AND value='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE created_by='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE created_by='OldAgent'") == 0);

    db_close(db);
    printf("  PASS: test_successful_rename\n");
}

static void test_busy_rejection(void) {
    sqlite3 *db = setup();
    /* Make session non-idle */
    sqlite3_exec(db, "UPDATE sessions SET state='running' WHERE agent_name='OldAgent'", NULL, NULL, NULL);

    int rc = agent_rename(db, "OldAgent", "NewAgent", 0);
    assert(rc == -1);
    /* Agent unchanged */
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='OldAgent'") == 1);

    db_close(db);
    printf("  PASS: test_busy_rejection\n");
}

static void test_busy_allows_requesting_session(void) {
    sqlite3 *db = setup();
    /* Get session id */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT id FROM sessions WHERE agent_name='OldAgent'", -1, &s, NULL);
    sqlite3_step(s);
    int64_t sid = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    /* Make it non-idle but pass it as requesting session */
    sqlite3_exec(db, "UPDATE sessions SET state='running' WHERE agent_name='OldAgent'", NULL, NULL, NULL);
    int rc = agent_rename(db, "OldAgent", "NewAgent", sid);
    assert(rc == 0);

    db_close(db);
    printf("  PASS: test_busy_allows_requesting_session\n");
}

static void test_name_conflict(void) {
    sqlite3 *db = setup();
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('Taken')", NULL, NULL, NULL);
    int rc = agent_rename(db, "OldAgent", "Taken", 0);
    assert(rc == -2);
    db_close(db);
    printf("  PASS: test_name_conflict\n");
}

static void test_invalid_name(void) {
    sqlite3 *db = setup();
    assert(agent_rename(db, "OldAgent", "has space", 0) == -3);
    assert(agent_rename(db, "OldAgent", "", 0) == -3);
    assert(agent_rename(db, "OldAgent", "a/b", 0) == -3);
    db_close(db);
    printf("  PASS: test_invalid_name\n");
}

static void test_not_found(void) {
    sqlite3 *db = setup();
    int rc = agent_rename(db, "Nonexistent", "NewName", 0);
    assert(rc == -4);
    db_close(db);
    printf("  PASS: test_not_found\n");
}

/* Schema-shape audit (schema-string-keys decision, 2026-07-18): the rename
 * cascade is FK-driven, so the structural invariant is "every agent_name
 * column carries a REFERENCES agents(name) ON UPDATE CASCADE clause". A new
 * table that forgets the clause silently reintroduces rename orphans — this
 * fails the build instead. */
static void test_fk_shape_audit(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT m.name FROM sqlite_master m"
        " WHERE m.type='table' AND m.name NOT LIKE 'sqlite_%' AND m.name<>'agents'"
        "   AND EXISTS(SELECT 1 FROM pragma_table_info(m.name) WHERE name='agent_name')"
        "   AND NOT EXISTS(SELECT 1 FROM pragma_foreign_key_list(m.name)"
        "                  WHERE \"from\"='agent_name' AND \"table\"='agents'"
        "                    AND \"to\"='name' AND on_update='CASCADE')",
        -1, &s, NULL) == SQLITE_OK);
    int bad = 0;
    while (sqlite3_step(s) == SQLITE_ROW) {
        printf("  MISSING FK: %s.agent_name lacks REFERENCES agents(name)"
               " ON UPDATE CASCADE\n", (const char *)sqlite3_column_text(s, 0));
        bad++;
    }
    sqlite3_finalize(s);
    assert(bad == 0);

    /* created_by is the same invariant on agents itself */
    assert(count_rows(db,
        "SELECT COUNT(*) FROM pragma_foreign_key_list('agents')"
        " WHERE \"from\"='created_by' AND \"table\"='agents'"
        "   AND \"to\"='name' AND on_update='CASCADE'") == 1);

    db_close(db);
    printf("  PASS: test_fk_shape_audit\n");
}

/* Behavioral belt-and-braces for the shape audit above: enumerate every
 * agent_name table from the live schema, seed a row in each generically,
 * rename, and assert nothing survives under the old name. */
static void seed_generic_row(sqlite3 *db, const char *table, const char *agent) {
    /* Build INSERT(cols) VALUES(vals) from pragma_table_info: agent_name gets
     * `agent`, other NOT-NULL-without-default non-autoincrement columns get a
     * type-appropriate dummy, everything else is left to its default. */
    char q[256];
    snprintf(q, sizeof(q), "PRAGMA table_info('%s')", table);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, q, -1, &s, NULL) == SQLITE_OK);
    char cols[1024] = "", vals[1024] = "";
    while (sqlite3_step(s) == SQLITE_ROW) {
        /* pragma_table_info cols: 0=cid 1=name 2=type 3=notnull 4=dflt 5=pk.
         * A single INTEGER PRIMARY KEY reports notnull=0 (rowid alias), so the
         * `!notnull` skip covers autoincrement; a composite/text PK member is
         * notnull=1 and must still get a value. */
        const char *name = (const char *)sqlite3_column_text(s, 1);
        const char *type = (const char *)sqlite3_column_text(s, 2);
        int notnull = sqlite3_column_int(s, 3);
        int has_dflt = sqlite3_column_type(s, 4) != SQLITE_NULL;
        char val[64];
        if (strcmp(name, "agent_name") == 0) {
            snprintf(val, sizeof(val), "'%s'", agent);
        } else if (strcmp(name, "model_id") == 0) {
            /* FK into models: seed the referenced row once. */
            sqlite3_exec(db,
                "INSERT OR IGNORE INTO providers(name,base_url) VALUES('p','http://p');"
                "INSERT OR IGNORE INTO models(id,provider_name,model)"
                " VALUES('p/m','p','m');", NULL, NULL, NULL);
            snprintf(val, sizeof(val), "'p/m'");
        } else if (has_dflt || !notnull) {
            continue;  /* defaulted / nullable / autoincrement rowid — skip */
        } else if (type && (strstr(type, "INT") || strstr(type, "int"))) {
            snprintf(val, sizeof(val), "0");
        } else {
            snprintf(val, sizeof(val), "'x'");
        }
        if (cols[0]) { strncat(cols, ",", sizeof(cols)-strlen(cols)-1);
                       strncat(vals, ",", sizeof(vals)-strlen(vals)-1); }
        strncat(cols, name, sizeof(cols)-strlen(cols)-1);
        strncat(vals, val, sizeof(vals)-strlen(vals)-1);
    }
    sqlite3_finalize(s);
    char ins[2304];
    snprintf(ins, sizeof(ins), "INSERT INTO %s(%s) VALUES(%s)", table, cols, vals);
    char *err = NULL;
    int rc = sqlite3_exec(db, ins, NULL, NULL, &err);
    /* Every agent_name table must be seedable this way; a failure means the
     * generic seeder needs a case, not that the table is exempt. */
    if (rc != SQLITE_OK) {
        printf("  seed failed for %s: %s\n  (%s)\n", table, err ? err : "?", ins);
        assert(0 && "generic seed failed — extend seed_generic_row");
    }
    sqlite3_free(err);
}

static void test_rename_no_orphans_self_audit(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('OldAgent')", NULL, NULL, NULL);

    /* Collect every table carrying an agent_name column (except agents, whose
     * key is `name`, handled by the rename separately). */
    char tables[32][64];
    int nt = 0;
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT m.name FROM sqlite_master m"
        " WHERE m.type='table' AND m.name NOT LIKE 'sqlite_%'"
        "   AND EXISTS(SELECT 1 FROM pragma_table_info(m.name) WHERE name='agent_name')"
        "   AND m.name<>'agents'"
        " ORDER BY m.name", -1, &s, NULL) == SQLITE_OK);
    while (sqlite3_step(s) == SQLITE_ROW && nt < 32)
        snprintf(tables[nt++], 64, "%s", (const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(nt > 0);

    for (int i = 0; i < nt; i++)
        seed_generic_row(db, tables[i], "OldAgent");

    assert(agent_rename(db, "OldAgent", "NewAgent", 0) == 0);

    /* No agent_name table may retain a row under the old name. */
    for (int i = 0; i < nt; i++) {
        char q[128];
        snprintf(q, sizeof(q),
                 "SELECT COUNT(*) FROM %s WHERE agent_name='OldAgent'", tables[i]);
        int orphans = count_rows(db, q);
        if (orphans != 0)
            printf("  ORPHAN: %d row(s) left in %s under OldAgent"
                   " — its agent_name FK isn't cascading\n", orphans, tables[i]);
        assert(orphans == 0);
    }
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='OldAgent'") == 0);

    db_close(db);
    printf("  PASS: test_rename_no_orphans_self_audit (%d agent_name tables)\n", nt);
}

/* ── v47: target_agent cascade, quiesce lease, two-domain rename ── */

/* cron_jobs.target_agent gained its FK in v47 — a 'new'-mode job pointing at
 * the renamed agent must follow it, exactly like agent_name does. */
static void test_target_agent_cascade(void) {
    sqlite3 *db = setup();
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('Runner')", NULL, NULL, NULL);
    sqlite3_exec(db,
        "INSERT INTO cron_jobs(agent_name,name,cron_expr,session_id,task,"
        " target,target_agent) VALUES('Runner','j2','* * * * *',1,'hi',"
        " 'new','OldAgent')", NULL, NULL, NULL);
    assert(agent_rename(db, "OldAgent", "NewAgent", 0) == 0);
    assert(count_rows(db,
        "SELECT COUNT(*) FROM cron_jobs WHERE target_agent='NewAgent'") == 1);
    assert(count_rows(db,
        "SELECT COUNT(*) FROM cron_jobs WHERE target_agent='OldAgent'") == 0);
    db_close(db);
    printf("  PASS: test_target_agent_cascade\n");
}

/* The lease is a CAS: the second taker loses, and only the holder can
 * refresh or release it. An expired lease is free for the taking. */
static void test_hold_lease(void) {
    sqlite3 *db = setup();
    assert(agent_hold_acquire(db, "OldAgent", 60, "cli:1") == 0);
    assert(agent_hold_acquire(db, "OldAgent", 60, "cli:2") == -1);
    assert(agent_hold_refresh(db, "OldAgent", 60, "cli:2") == -1);
    assert(agent_hold_refresh(db, "OldAgent", 60, "cli:1") == 0);
    assert(agent_hold_release(db, "OldAgent", "cli:2") == -1);
    assert(agent_hold_release(db, "OldAgent", "cli:1") == 0);
    assert(agent_hold_acquire(db, "OldAgent", 60, "cli:2") == 0);

    /* Expired lease self-heals — no cleanup path needed for a dead holder. */
    sqlite3_exec(db, "UPDATE agents SET hold_until=unixepoch()-1"
                     " WHERE name='OldAgent'", NULL, NULL, NULL);
    assert(agent_hold_acquire(db, "OldAgent", 60, "cli:3") == 0);

    /* Missing agent is indistinguishable from contention at the SQL level;
     * both are "you don't hold it". */
    assert(agent_hold_acquire(db, "Ghost", 60, "cli:1") == -1);
    db_close(db);
    printf("  PASS: test_hold_lease\n");
}

/* The drain predicate: a busy session only counts while its owner is alive. */
static void test_busy_session_liveness(void) {
    sqlite3 *db = setup();
    char state[32];
    assert(agent_busy_session(db, "OldAgent", state, sizeof(state)) == 0);

    sqlite3_exec(db, "UPDATE sessions SET state='llm_running',"
                     " owner_instance='dead-1' WHERE agent_name='OldAgent'",
                 NULL, NULL, NULL);
    /* Dead owner (no processes row) reads as idle — otherwise a rename waits
     * for a daemon that will never finish the turn. */
    assert(agent_busy_session(db, "OldAgent", state, sizeof(state)) == 0);

    sqlite3_exec(db, "INSERT INTO processes(instance_id,pid,mode)"
                     " VALUES('dead-1',1,'daemon')", NULL, NULL, NULL);
    int64_t sid = agent_busy_session(db, "OldAgent", state, sizeof(state));
    assert(sid > 0);
    assert(strcmp(state, "llm_running") == 0);

    sqlite3_exec(db, "UPDATE sessions SET state='idle', owner_instance=NULL",
                 NULL, NULL, NULL);
    assert(agent_busy_session(db, "OldAgent", state, sizeof(state)) == 0);
    db_close(db);
    printf("  PASS: test_busy_session_liveness\n");
}

/* Two storage domains, one logical change: when the disk move can't happen,
 * the committed DB rename is compensated so both domains agree again. */
static void test_rename_full_disk_rollback(void) {
    sqlite3 *db = setup();
    char root[128];
    snprintf(root, sizeof(root), "/tmp/test_rename_full_%ld", (long)getpid());
    char old_dir[192], new_dir[192], cmd[1024];
    snprintf(old_dir, sizeof(old_dir), "%s/OldAgent", root);
    snprintf(new_dir, sizeof(new_dir), "%s/NewAgent", root);
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s && mkdir -p %s",
             root, old_dir, new_dir);
    assert(system(cmd) == 0);

    /* NewAgent/ already exists on disk: moving onto it would merge two
     * agents' workspaces, so the rename must refuse and undo the DB step. */
    assert(agent_rename_full(db, "OldAgent", "NewAgent", 0, root) == -1);
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='OldAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='NewAgent'") == 0);
    assert(access(old_dir, F_OK) == 0);

    /* Clear the obstacle and the same call succeeds in both domains. */
    snprintf(cmd, sizeof(cmd), "rmdir %s", new_dir);
    assert(system(cmd) == 0);
    assert(agent_rename_full(db, "OldAgent", "NewAgent", 0, root) == 0);
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='NewAgent'") == 1);
    assert(access(new_dir, F_OK) == 0);
    assert(access(old_dir, F_OK) != 0);

    snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
    assert(system(cmd) == 0);
    db_close(db);
    printf("  PASS: test_rename_full_disk_rollback\n");
}

int main(void) {
    TEST_INIT();
    printf("test_agent_rename:\n");
    test_successful_rename();
    test_fk_shape_audit();
    test_rename_no_orphans_self_audit();
    test_busy_rejection();
    test_busy_allows_requesting_session();
    test_name_conflict();
    test_invalid_name();
    test_not_found();
    test_target_agent_cascade();
    test_hold_lease();
    test_busy_session_liveness();
    test_rename_full_disk_rollback();
    printf("all agent_rename tests passed\n");
    return 0;
}
