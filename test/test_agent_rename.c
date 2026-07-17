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
    /* Seed an agent */
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('OldAgent')", NULL, NULL, NULL);
    /* Seed related rows */
    sqlite3_exec(db, "INSERT INTO sessions(name,agent_name,state) VALUES('s1','OldAgent','idle')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO agent_extensions(agent_name,extension_name) VALUES('OldAgent','telegram')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO channel_routes(channel_name,channel_id,agent_name) VALUES('tg','*','OldAgent')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO cron_jobs(agent_name,name,cron_expr,session_id,task) VALUES('OldAgent','job1','* * * * *',1,'hi')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO memory_blocks(agent_name,label,value) VALUES('OldAgent','AGENT','hello')", NULL, NULL, NULL);
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
    assert(count_rows(db, "SELECT COUNT(*) FROM channel_routes WHERE agent_name='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM cron_jobs WHERE agent_name='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM memory_blocks WHERE agent_name='NewAgent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM config WHERE key='default_agent' AND value='NewAgent'") == 1);

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

/* Self-auditing cascade check (schema-string-keys project, step 4): the
 * rename cascade is a hardcoded list, so a new table with an agent_name
 * column added to schema.sql without a matching UPDATE silently orphans on
 * rename. Enumerate every agent_name table from the live schema, seed a row
 * in each generically, rename, and assert nothing survives under the old
 * name — turning the manual checklist into a failing test. */
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
                   " — add it to agent_rename's cascade\n", orphans, tables[i]);
        assert(orphans == 0);
    }
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='OldAgent'") == 0);

    db_close(db);
    printf("  PASS: test_rename_no_orphans_self_audit (%d agent_name tables)\n", nt);
}

int main(void) {
    TEST_INIT();
    printf("test_agent_rename:\n");
    test_successful_rename();
    test_rename_no_orphans_self_audit();
    test_busy_rejection();
    test_busy_allows_requesting_session();
    test_name_conflict();
    test_invalid_name();
    test_not_found();
    printf("all agent_rename tests passed\n");
    return 0;
}
