#define _POSIX_C_SOURCE 200809L
#include "db_response.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *test_db(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    /* Create tool_calls table (normally done by db_open_agent) */
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS tool_calls ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL,"
        "  entry_id INTEGER NOT NULL,"
        "  call_id TEXT NOT NULL,"
        "  name TEXT NOT NULL,"
        "  arguments TEXT,"
        "  status TEXT NOT NULL DEFAULT 'pending',"
        "  result_entry_id INTEGER"
        ");", NULL, NULL, NULL);
    return db;
}

static int count_rows(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s; int n = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) n = sqlite3_column_int(s, 0);
        sqlite3_finalize(s);
    }
    return n;
}

/* Test: db_ingest_response writes entries + tool_calls from an OpenAI body */
static void test_ingest_response(void) {
    printf("  test_ingest_response...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message msg = {.role = ROLE_USER, .content = "do it"};
    entry_append_with_turn(db, sid, &msg, 1);

    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"Let me list files.\","
        "\"tool_calls\":[{\"id\":\"call_typed\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell_exec\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":50,\"cost\":0.0000015}}";

    TypedIngestResult result;
    LlmRespStatus st = db_ingest_response(db, sid, 1, "deepseek-v4", ENDPOINT_OPENAI,
                                          body, NULL, 1, &result);
    assert(st == LLM_RESP_OK);
    assert(result.assistant_entry_id > 0);
    assert(result.prompt_tokens == 100);
    assert(result.completion_tokens == 50);
    assert(result.cost_nano == 1500);

    /* Verify tool_calls table (workflow state only — args in entries.content) */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db,
        "SELECT call_id, name FROM tool_calls WHERE call_id='call_typed';",
        -1, &stmt, NULL);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "call_typed") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "shell_exec") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    printf(" OK\n");
}

/* Test: malformed body (no choices) returns LLM_RESP_MALFORMED, writes nothing */
static void test_ingest_malformed(void) {
    printf("  test_ingest_malformed...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    TypedIngestResult result;
    LlmRespStatus st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI,
                                          "{\"error\":\"nope\"}", NULL, 1, &result);
    assert(st == LLM_RESP_MALFORMED);

    st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, "not json", NULL, 1, &result);
    assert(st == LLM_RESP_MALFORMED);

    st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, "{\"choices\":[]}", NULL, 1, &result);
    assert(st == LLM_RESP_MALFORMED);

    db_close(db);
    printf(" OK\n");
}

/* Test: db_ingest_response archives the raw body to llm_responses as JSONB */
static void test_ingest_archive(void) {
    printf("  test_ingest_archive...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *body =
        "{\"id\":\"resp_abc\",\"choices\":[{\"message\":{\"content\":\"hi\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";
    TypedIngestResult ir;
    assert(db_ingest_response(db, sid, 7, "m", ENDPOINT_OPENAI, body, NULL, 1, &ir) == LLM_RESP_OK);

    /* Row archived: status, provider id, turn_id, and a re-queryable JSONB body. */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT status, provider_id, turn_id, typeof(body),"
        " json_extract(body,'$.choices[0].message.content')"
        " FROM llm_responses WHERE session_id=? ORDER BY id DESC LIMIT 1;", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "ok") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "resp_abc") == 0);
    assert(sqlite3_column_int64(s, 2) == 7);
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "blob") == 0);   /* stored as JSONB */
    assert(strcmp((const char *)sqlite3_column_text(s, 4), "hi") == 0);     /* re-queryable */
    sqlite3_finalize(s);

    /* Not-JSON body archives as status='malformed' with the raw text. */
    assert(db_ingest_response(db, sid, 8, "m", ENDPOINT_OPENAI, "<html>nope", NULL, 1, &ir) == LLM_RESP_MALFORMED);
    sqlite3_prepare_v2(db,
        "SELECT status, typeof(body), body FROM llm_responses WHERE turn_id=8;", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "malformed") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "text") == 0);   /* raw text, not JSONB */
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "<html>nope") == 0);
    sqlite3_finalize(s);

    db_close(db);
    printf(" OK\n");
}

/* Test: db_archive_response (any HTTP outcome) + configurable retention */
static void test_archive_retention(void) {
    printf("  test_archive_retention...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* An error body archives under its label; JSON gets provider_id extracted.
     * The request body we sent is archived alongside (recoverable for debugging). */
    db_archive_response(db, sid, 1, "m", "http_500",
                        "{\"id\":\"err_1\",\"error\":{\"message\":\"boom\"}}",
                        "{\"model\":\"m\",\"messages\":[{\"role\":\"user\"}]}");
    db_archive_response(db, sid, 2, "m", "timeout", "upstream timed out", NULL);  /* not JSON */

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT status, provider_id, typeof(body),"
        " json_extract(request_body,'$.model') FROM llm_responses WHERE turn_id=1;", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "http_500") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "err_1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "blob") == 0);   /* JSONB */
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "m") == 0);      /* request archived */
    sqlite3_finalize(s);
    sqlite3_prepare_v2(db,
        "SELECT status, typeof(body) FROM llm_responses WHERE turn_id=2;", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "timeout") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "text") == 0);   /* raw text */
    sqlite3_finalize(s);

    /* Cap = 2, per status class: a stream of 'ok' rows prunes older 'ok' rows
     * but never pushes out the failures that error entries cite ("[resp #N]").
     * The two failures above (http_500, timeout) must both survive. */
    sqlite3_exec(db, "INSERT OR REPLACE INTO config(key,value) VALUES('llm_response_archive_max','2');",
                 NULL, NULL, NULL);
    for (int i = 0; i < 5; i++)
        db_archive_response(db, sid, 100 + i, "m", "ok", "{\"x\":1}", NULL);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses WHERE status='ok';") == 2);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses WHERE status!='ok';") == 2);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses WHERE turn_id IN (1,2);") == 2);

    /* Failures prune within their own class too: two more push out the first two. */
    db_archive_response(db, sid, 110, "m", "http_502", "oops", NULL);
    db_archive_response(db, sid, 111, "m", "network_error", NULL, NULL);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses WHERE status!='ok';") == 2);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses WHERE turn_id IN (1,2);") == 0);

    /* Cap = -1: pruning disabled, rows accumulate. */
    sqlite3_exec(db, "UPDATE config SET value='-1' WHERE key='llm_response_archive_max';",
                 NULL, NULL, NULL);
    for (int i = 0; i < 5; i++)
        db_archive_response(db, sid, 200 + i, "m", "ok", "{\"x\":1}", NULL);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses;") == 9);

    /* Cap = 0: archiving off — nothing written (no churn, count unchanged). */
    sqlite3_exec(db, "UPDATE config SET value='0' WHERE key='llm_response_archive_max';",
                 NULL, NULL, NULL);
    for (int i = 0; i < 3; i++)
        db_archive_response(db, sid, 300 + i, "m", "ok", "{\"x\":1}", NULL);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses;") == 9);

    db_close(db);
    printf(" OK\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_db_response:\n");
    test_ingest_response();
    test_ingest_malformed();
    test_ingest_archive();
    test_archive_retention();
    printf("  ALL PASSED\n");
    return 0;
}
