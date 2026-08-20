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
    entry_append_with_iteration(db, sid, &msg, 1);

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

    /* Row archived: status, provider id, iteration_id, and a re-queryable JSONB body. */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT status, provider_id, iteration_id, typeof(body),"
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
        "SELECT status, typeof(body), body FROM llm_responses WHERE iteration_id=8;", -1, &s, NULL);
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
        " json_extract(request_body,'$.model') FROM llm_responses WHERE iteration_id=1;", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "http_500") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "err_1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "blob") == 0);   /* JSONB */
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "m") == 0);      /* request archived */
    sqlite3_finalize(s);
    sqlite3_prepare_v2(db,
        "SELECT status, typeof(body) FROM llm_responses WHERE iteration_id=2;", -1, &s, NULL);
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
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id IN (1,2);") == 2);

    /* Failures prune within their own class too: two more push out the first two. */
    db_archive_response(db, sid, 110, "m", "http_502", "oops", NULL);
    db_archive_response(db, sid, 111, "m", "network_error", NULL, NULL);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses WHERE status!='ok';") == 2);
    assert(count_rows(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id IN (1,2);") == 0);

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

/* One scalar out of the reasoning entry's meta blob, or NULL. */
static const char *meta_str(sqlite3 *db, const char *path, sqlite3_stmt **out) {
    char sql[192];
    snprintf(sql, sizeof(sql),
             "SELECT json_extract(reasoning_meta,'%s') FROM entries"
             " WHERE type='reasoning' ORDER BY id DESC LIMIT 1", path);
    sqlite3_prepare_v2(db, sql, -1, out, NULL);
    if (sqlite3_step(*out) == SQLITE_ROW)
        return (const char *)sqlite3_column_text(*out, 0);
    return NULL;
}

/* Capture: OpenRouter reasoning_details is stored verbatim and tagged with the
 * producing provider+model — even with save_reasoning off, because the replay
 * requirement is the provider's, not the operator's. */
static void test_capture_reasoning_details(void) {
    printf("  test_capture_reasoning_details...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"listing\",\"reasoning\":\"because\","
        "\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"because\","
        "\"format\":\"google-gemini-v1\",\"signature\":\"SIG123\"}],"
        "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell_exec\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";
    assert(db_ingest_response(db, sid, 1, "gemini-3-flash", ENDPOINT_OPENAI,
                              body, NULL, 0, NULL) == LLM_RESP_OK);

    sqlite3_stmt *s;
    assert(strcmp(meta_str(db, "$.format", &s), "reasoning_details") == 0);
    sqlite3_finalize(s);
    assert(strcmp(meta_str(db, "$.model", &s), "gemini-3-flash") == 0);
    sqlite3_finalize(s);
    assert(strcmp(meta_str(db, "$.provider", &s), "openai") == 0);
    sqlite3_finalize(s);
    assert(strcmp(meta_str(db, "$.blob[0].signature", &s), "SIG123") == 0);
    sqlite3_finalize(s);
    /* save_reasoning=0: the text is discarded, the blob is not. */
    assert(count_rows(db,
        "SELECT COUNT(*) FROM entries WHERE type='reasoning' AND content IS NULL") == 1);

    /* Bare-string shape (DeepSeek): the string itself is the blob. */
    const char *ds =
        "{\"choices\":[{\"message\":{\"content\":\"ok\","
        "\"reasoning_content\":\"deep thought\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";
    assert(db_ingest_response(db, sid, 2, "deepseek-v4", ENDPOINT_OPENAI,
                              ds, NULL, 1, NULL) == LLM_RESP_OK);
    assert(strcmp(meta_str(db, "$.format", &s), "reasoning_content") == 0);
    sqlite3_finalize(s);
    assert(strcmp(meta_str(db, "$.blob", &s), "deep thought") == 0);
    sqlite3_finalize(s);

    db_close(db);
    printf(" OK\n");
}

/* Capture: Gemini-native thoughtSignature, keyed by the functionCall it rode. */
static void test_capture_gemini_signature(void) {
    printf("  test_capture_gemini_signature...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *body =
        "{\"candidates\":[{\"content\":{\"parts\":["
        "{\"text\":\"ok\"},"
        "{\"functionCall\":{\"name\":\"shell_exec\",\"args\":{\"cmd\":\"ls\"}},"
        " \"thoughtSignature\":\"GSIG\"}]},\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":1,\"candidatesTokenCount\":1}}";
    assert(db_ingest_response(db, sid, 1, "gemini-3-pro", ENDPOINT_GEMINI,
                              body, NULL, 0, NULL) == LLM_RESP_OK);

    sqlite3_stmt *s;
    assert(strcmp(meta_str(db, "$.format", &s), "gemini_parts") == 0);
    sqlite3_finalize(s);
    assert(strcmp(meta_str(db, "$.blob[0].fn", &s), "shell_exec") == 0);
    sqlite3_finalize(s);
    assert(strcmp(meta_str(db, "$.blob[0].sig", &s), "GSIG") == 0);
    sqlite3_finalize(s);

    /* No signature anywhere → no reasoning entry at all. */
    const char *plain =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":1,\"candidatesTokenCount\":1}}";
    assert(db_ingest_response(db, sid, 2, "gemini-3-pro", ENDPOINT_GEMINI,
                              plain, NULL, 1, NULL) == LLM_RESP_OK);
    assert(count_rows(db, "SELECT COUNT(*) FROM entries WHERE type='reasoning'") == 1);

    db_close(db);
    printf(" OK\n");
}

int main(void) {
    TEST_INIT();
    printf("test_db_response:\n");
    test_ingest_response();
    test_ingest_malformed();
    test_ingest_archive();
    test_archive_retention();
    test_capture_reasoning_details();
    test_capture_gemini_signature();
    printf("  ALL PASSED\n");
    return 0;
}
