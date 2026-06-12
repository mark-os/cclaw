#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_agent_rename.db"

static sqlite3 *setup(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    /* Seed an agent */
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('old-agent')", NULL, NULL, NULL);
    /* Seed related rows */
    sqlite3_exec(db, "INSERT INTO sessions(name,agent_name,state) VALUES('s1','old-agent','idle')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO agent_extensions(agent_name,extension_name) VALUES('old-agent','telegram')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO channel_routes(channel_name,channel_id,agent_name) VALUES('tg','*','old-agent')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO cron_jobs(agent_name,name,cron_expr,session_id,task) VALUES('old-agent','job1','* * * * *',1,'hi')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO memory_blocks(agent_name,label,value) VALUES('old-agent','AGENT','hello')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO config(key,value) VALUES('default_agent','old-agent')", NULL, NULL, NULL);
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
    int rc = agent_rename(db, "old-agent", "new-agent", 0);
    assert(rc == 0);

    /* Verify cascade */
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='new-agent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='old-agent'") == 0);
    assert(count_rows(db, "SELECT COUNT(*) FROM sessions WHERE agent_name='new-agent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM agent_extensions WHERE agent_name='new-agent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM channel_routes WHERE agent_name='new-agent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM cron_jobs WHERE agent_name='new-agent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM memory_blocks WHERE agent_name='new-agent'") == 1);
    assert(count_rows(db, "SELECT COUNT(*) FROM config WHERE key='default_agent' AND value='new-agent'") == 1);

    db_close(db);
    printf("  PASS: test_successful_rename\n");
}

static void test_busy_rejection(void) {
    sqlite3 *db = setup();
    /* Make session non-idle */
    sqlite3_exec(db, "UPDATE sessions SET state='running' WHERE agent_name='old-agent'", NULL, NULL, NULL);

    int rc = agent_rename(db, "old-agent", "new-agent", 0);
    assert(rc == -1);
    /* Agent unchanged */
    assert(count_rows(db, "SELECT COUNT(*) FROM agents WHERE name='old-agent'") == 1);

    db_close(db);
    printf("  PASS: test_busy_rejection\n");
}

static void test_busy_allows_requesting_session(void) {
    sqlite3 *db = setup();
    /* Get session id */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT id FROM sessions WHERE agent_name='old-agent'", -1, &s, NULL);
    sqlite3_step(s);
    int64_t sid = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);

    /* Make it non-idle but pass it as requesting session */
    sqlite3_exec(db, "UPDATE sessions SET state='running' WHERE agent_name='old-agent'", NULL, NULL, NULL);
    int rc = agent_rename(db, "old-agent", "new-agent", sid);
    assert(rc == 0);

    db_close(db);
    printf("  PASS: test_busy_allows_requesting_session\n");
}

static void test_name_conflict(void) {
    sqlite3 *db = setup();
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('taken')", NULL, NULL, NULL);
    int rc = agent_rename(db, "old-agent", "taken", 0);
    assert(rc == -2);
    db_close(db);
    printf("  PASS: test_name_conflict\n");
}

static void test_invalid_name(void) {
    sqlite3 *db = setup();
    assert(agent_rename(db, "old-agent", "has space", 0) == -3);
    assert(agent_rename(db, "old-agent", "", 0) == -3);
    assert(agent_rename(db, "old-agent", "a/b", 0) == -3);
    db_close(db);
    printf("  PASS: test_invalid_name\n");
}

static void test_not_found(void) {
    sqlite3 *db = setup();
    int rc = agent_rename(db, "nonexistent", "new-name", 0);
    assert(rc == -4);
    db_close(db);
    printf("  PASS: test_not_found\n");
}

int main(void) {
    printf("test_agent_rename:\n");
    test_successful_rename();
    test_busy_rejection();
    test_busy_allows_requesting_session();
    test_name_conflict();
    test_invalid_name();
    test_not_found();
    printf("all agent_rename tests passed\n");
    return 0;
}
