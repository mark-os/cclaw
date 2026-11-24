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
}

/* Read-only: every write or DDL statement is refused before it runs. */
static void test_reject_writes(void) {
    static const char *writes[] = {
        "{\"sql\":\"INSERT INTO sessions VALUES(1,'x',0,NULL,NULL,NULL,NULL,0,'','')\"}",
        "{\"sql\":\"UPDATE sessions SET name='x'\"}",
        "{\"sql\":\"DELETE FROM sessions\"}",
        "{\"sql\":\"DROP TABLE sessions\"}",
    };
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i++) {
        printf("    %s\n", writes[i]);
        char *r = tool_db_query_handler(writes[i], db, &(int){0});
        assert(r && strstr(r, "error: only SELECT"));
        free(r);
    }
    db_close(db);
}

static void test_invalid_sql(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"sql\":\"SELECT * FROM nonexistent_table\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "error:"));
    free(r);
    db_close(db);
}

static void test_missing_sql_field(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    char *r = tool_db_query_handler("{\"query\":\"SELECT 1\"}", db, &(int){0});
    assert(r);
    assert(strstr(r, "error: missing"));
    free(r);
    db_close(db);
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
}

/* The admin token and the chat replies that carried its link never come out,
 * however the query is spelled — and the handle is left without the block. */
static void test_admin_token_withheld(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int rc = sqlite3_exec(db,
        "INSERT OR REPLACE INTO config(key, value) VALUES('web_admin_token','t0ken5ecret');"
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES('tg',1,'http://h/admin?token=t0ken5ecret');", NULL, NULL, NULL);
    assert(rc == SQLITE_OK);
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
    const char *tok_sql = "SELECT value FROM config WHERE key='web_admin_token'";
    char *v = test_qs(db, tok_sql);
    assert(v && strcmp(v, "t0ken5ecret") == 0);
    free(v);
    /* A failed prepare must not leave it installed either. */
    r = tool_db_query_handler("{\"sql\":\"SELECT nope FROM config\"}", db, &(int){0});
    free(r);
    v = test_qs(db, tok_sql);
    assert(v);
    free(v);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    printf("test_tool_db_query:\n");
    RUN(test_select_returns_results);
    RUN(test_reject_writes);
    RUN(test_invalid_sql);
    RUN(test_missing_sql_field);
    RUN(test_secret_filtering);
    RUN(test_admin_token_withheld);
    return 0;
}
