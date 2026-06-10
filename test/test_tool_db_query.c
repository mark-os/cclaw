#include "tool_db_query.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_select_returns_results(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT 1 AS val, 'hello' AS msg\"}", db);
    assert(r);
    /* Pipe-separated: header then data */
    assert(strstr(r, "val|msg\n"));
    assert(strstr(r, "1|hello\n"));
    free(r);
    db_close(db);
    printf("  PASS: select returns results\n");
}

static void test_reject_insert(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"INSERT INTO sessions VALUES(1,'x',0,NULL,NULL,NULL,NULL,0,'','')\"}", db);
    assert(r);
    assert(strstr(r, "error: only SELECT"));
    free(r);
    db_close(db);
    printf("  PASS: reject INSERT\n");
}

static void test_reject_update(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"UPDATE sessions SET name='x'\"}", db);
    assert(r);
    assert(strstr(r, "error: only SELECT"));
    free(r);
    db_close(db);
    printf("  PASS: reject UPDATE\n");
}

static void test_reject_delete(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"DELETE FROM sessions\"}", db);
    assert(r);
    assert(strstr(r, "error: only SELECT"));
    free(r);
    db_close(db);
    printf("  PASS: reject DELETE\n");
}

static void test_reject_drop(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"DROP TABLE sessions\"}", db);
    assert(r);
    assert(strstr(r, "error: only SELECT"));
    free(r);
    db_close(db);
    printf("  PASS: reject DROP\n");
}

static void test_invalid_sql(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT * FROM nonexistent_table\"}", db);
    assert(r);
    assert(strstr(r, "error:"));
    free(r);
    db_close(db);
    printf("  PASS: invalid SQL returns error\n");
}

static void test_missing_sql_field(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"query\":\"SELECT 1\"}", db);
    assert(r);
    assert(strstr(r, "error: missing"));
    free(r);
    db_close(db);
    printf("  PASS: missing sql field\n");
}

static void test_query_sessions_table(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    session_create(db, "test-session", NULL, -1, 0);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT id, name FROM sessions\"}", db);
    assert(r);
    assert(strstr(r, "id|name\n"));
    assert(strstr(r, "test-session"));
    free(r);
    db_close(db);
    printf("  PASS: query sessions table\n");
}

static void test_secret_filtering(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    sqlite3_exec(db, "INSERT OR REPLACE INTO config(key,value) VALUES('test.plain','visible')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO config(key,value) VALUES('test.secret','enc:abcdef1234567890')", NULL, NULL, NULL);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT key, value FROM config WHERE key LIKE 'test.%'\"}", db);
    assert(r);
    assert(strstr(r, "test.plain"));
    assert(strstr(r, "visible"));
    assert(!strstr(r, "test.secret"));
    assert(!strstr(r, "enc:"));
    free(r);
    db_close(db);
    printf("  PASS: secret filtering\n");
}

static void test_register(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_db_query_register(&reg, db);
    assert(rc == 0);

    ToolEntry *e = tools_lookup(&reg, "db_query");
    assert(e);
    assert(strcmp(e->name, "db_query") == 0);

    tools_free(&reg);
    db_close(db);
    printf("  PASS: register\n");
}

int main(void) {
    printf("test_tool_db_query:\n");
    test_select_returns_results();
    test_reject_insert();
    test_reject_update();
    test_reject_delete();
    test_reject_drop();
    test_invalid_sql();
    test_missing_sql_field();
    test_query_sessions_table();
    test_secret_filtering();
    test_register();
    printf("all tests passed\n");
    return 0;
}
