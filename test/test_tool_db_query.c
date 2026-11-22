#include "tool_db_query.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_select_returns_results(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT 1 AS val, 'hello' AS msg\"}", db, &(int){0});
    assert(r);
    /* Pipe-separated: header then data */
    assert(strstr(r, "val|msg\n"));
    assert(strstr(r, "1|hello\n"));
    free(r);
    db_close(db);
    printf("  PASS: select returns results\n");
}

static void test_reject_insert(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"INSERT INTO sessions VALUES(1,'x',0,NULL,NULL,NULL,NULL,0,'','')\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "error: only SELECT"));
    free(r);
    db_close(db);
    printf("  PASS: reject INSERT\n");
}

static void test_reject_update(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"UPDATE sessions SET name='x'\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "error: only SELECT"));
    free(r);
    db_close(db);
    printf("  PASS: reject UPDATE\n");
}

static void test_reject_delete(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"DELETE FROM sessions\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "error: only SELECT"));
    free(r);
    db_close(db);
    printf("  PASS: reject DELETE\n");
}

static void test_reject_drop(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"DROP TABLE sessions\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "error: only SELECT"));
    free(r);
    db_close(db);
    printf("  PASS: reject DROP\n");
}

static void test_invalid_sql(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT * FROM nonexistent_table\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "error:"));
    free(r);
    db_close(db);
    printf("  PASS: invalid SQL returns error\n");
}

static void test_missing_sql_field(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"query\":\"SELECT 1\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "error: missing"));
    free(r);
    db_close(db);
    printf("  PASS: missing sql field\n");
}

static void test_query_sessions_table(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    session_create(db, "test-session", NULL, -1, 0);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT id, name FROM sessions\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "id|name\n"));
    assert(strstr(r, "test-session"));
    free(r);
    db_close(db);
    printf("  PASS: query sessions table\n");
}

static void test_secret_filtering(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    /* Encrypted values live in `secrets`; a row carrying one is dropped whole. */
    sqlite3_exec(db, "INSERT INTO secrets(name,value) VALUES('A_SECRET','enc:abcdef1234567890')", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO sessions(name) VALUES('visible')", NULL, NULL, NULL);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT name, value FROM secrets UNION ALL SELECT name, 'x' FROM sessions\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "visible"));
    assert(!strstr(r, "A_SECRET"));
    assert(!strstr(r, "enc:"));
    assert(strstr(r, "1 row withheld"));
    free(r);
    db_close(db);
    printf("  PASS: secret filtering\n");
}

static void test_register(void) {
    sqlite3 *db = test_db_open(":memory:");
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

/* The admin token and the chat replies that carried its link never come out,
 * however the query is spelled — and the handle is left without the block. */
static void test_admin_token_withheld(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    assert(sqlite3_exec(db,
        "INSERT OR REPLACE INTO config(key, value) VALUES('web_admin_token','t0ken5ecret');"
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES('tg',1,'http://h/admin?token=t0ken5ecret');", NULL, NULL, NULL) == SQLITE_OK);
    const char *spellings[] = {
        "{\"sql\":\"SELECT * FROM config\"}",
        "{\"sql\":\"SELECT hex(value) FROM config WHERE key='web_admin_token'\"}",
        "{\"sql\":\"SELECT substr(v,2) FROM (SELECT value AS v FROM main.config)\"}",
        "{\"sql\":\"SELECT (SELECT group_concat(value) FROM config) AS x\"}",
        "{\"sql\":\"SELECT payload FROM channel_outbox\"}",
        "{\"sql\":\"SELECT * FROM (SELECT * FROM channel_outbox) AS c\"}",
    };
    for (size_t i = 0; i < sizeof(spellings) / sizeof(*spellings); i++) {
        char *r = tool_db_query_handler(spellings[i], db, &(int){0});
        assert(r);
        assert(!strstr(r, "t0ken5ecret"));
        assert(!strstr(r, "306B656E"));   /* hex of "0ken" */
        assert(strstr(r, "search_config"));
        free(r);
    }
    /* Keys and other columns still read; other tables say nothing of it. */
    char *r = tool_db_query_handler(
        "{\"sql\":\"SELECT key FROM config WHERE key='web_admin_token'\"}", db, &(int){0});
    assert(r && strstr(r, "web_admin_token") && !strstr(r, "search_config"));
    free(r);
    /* The block is this call's only: the same handle reads the value after. */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT value FROM config WHERE key='web_admin_token'",
                              -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(st, 0), "t0ken5ecret") == 0);
    sqlite3_finalize(st);
    /* A failed prepare must not leave it installed either. */
    r = tool_db_query_handler("{\"sql\":\"SELECT nope FROM config\"}", db, &(int){0});
    free(r);
    assert(sqlite3_prepare_v2(db, "SELECT value FROM config WHERE key='web_admin_token'",
                              -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0) != NULL);
    sqlite3_finalize(st);
    db_close(db);
    printf("  PASS: admin token withheld\n");
}

int main(void) {
    TEST_INIT();
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
    test_admin_token_withheld();
    test_register();
    printf("all tests passed\n");
    return 0;
}
