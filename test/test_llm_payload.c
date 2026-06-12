#include "db.h"
#include "test_util.h"
#include "llm_payload.h"
#include "config.h"
#include "context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_payload.db";

static void setup_session(sqlite3 *db, int64_t *sid) {
    *sid = session_create(db, "test", "default", -1, 0);
    assert(*sid > 0);

    /* User message with special chars */
    Message user = {.role = ROLE_USER, .content = "Hello \"world\"\nLine2\ttab"};
    entry_append_with_turn(db, *sid, &user, 1);

    /* Assistant reply */
    Message asst = {.role = ROLE_ASSISTANT, .content = "Hi there!"};
    entry_append_with_turn(db, *sid, &asst, 1);
}

/* Helper: extract string from JSON using sqlite */
static const char *json_get_str(sqlite3 *db, const char *json, const char *path, sqlite3_stmt **out) {
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT json_extract(?1,'%s')", path);
    sqlite3_prepare_v2(db, sql, -1, out, NULL);
    sqlite3_bind_text(*out, 1, json, -1, SQLITE_STATIC);
    if (sqlite3_step(*out) == SQLITE_ROW)
        return (const char *)sqlite3_column_text(*out, 0);
    return NULL;
}

static void test_openai_payload(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.provider.context_window = 128000;
    cfg.stream = 1;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(plan.count >= 2);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    assert(payload.body);

    /* Verify using SQLite json_extract */
    sqlite3_stmt *s;

    /* Check model */
    json_get_str(db, payload.body, "$.model", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "test-model") == 0);
    sqlite3_finalize(s);

    /* Check messages is array with >= 3 elements */
    sqlite3_prepare_v2(db, "SELECT json_array_length(json_extract(?1,'$.messages'))", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) >= 3);
    sqlite3_finalize(s);

    /* First should be system */
    json_get_str(db, payload.body, "$.messages[0].role", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "system") == 0);
    sqlite3_finalize(s);

    /* Second should be user with escaped content */
    json_get_str(db, payload.body, "$.messages[1].content", &s);
    const char *c = (const char *)sqlite3_column_text(s, 0);
    assert(c && strstr(c, "Hello \"world\""));
    sqlite3_finalize(s);

    /* Check stream flag */
    sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.stream')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);

    /* Check max_tokens */
    sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.max_tokens')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1024);
    sqlite3_finalize(s);

    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_openai_payload\n");
}

/* Regression: stream=0 / max_tokens=0 / no tools must OMIT the keys entirely.
 * json_object emits JSON null for SQL NULL; strict providers (DeepSeek) 400
 * on "stream":null. */
static void test_openai_payload_no_stream_omits_nulls(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 0;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.provider.context_window = 128000;
    cfg.stream = 0;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    assert(payload.body);

    /* None of the optional keys may exist (not even as JSON null) */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM json_each(?1)"
        " WHERE key IN ('stream','max_tokens','tools')",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 0);
    sqlite3_finalize(s);

    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_openai_payload_no_stream_omits_nulls\n");
}

static void test_gemini_payload(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    Config cfg = {0};
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.max_tokens = 2048;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.provider.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    assert(payload.body);

    sqlite3_stmt *s;

    /* Should have systemInstruction */
    sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.systemInstruction')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_type(s, 0) != SQLITE_NULL);
    sqlite3_finalize(s);

    /* Should have contents array */
    sqlite3_prepare_v2(db, "SELECT json_array_length(json_extract(?1,'$.contents'))", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) > 0);
    sqlite3_finalize(s);

    /* Verify no system role in contents */
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM json_each(json_extract(?1,'$.contents'))"
        " WHERE json_extract(value,'$.role')='system'",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 0);
    sqlite3_finalize(s);

    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_gemini_payload\n");
}

/* Recall must ride at the TAIL as a user message on both endpoints (keeps
 * the prompt prefix byte-stable for provider caching; never in the system
 * prompt / systemInstruction). */
static void test_recall_tail_user_message(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    const char *recall = "---Possibly relevant context---\n[session 3, user] hi\n---End of context---";

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.provider.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, recall, "You are helpful.", &payload) == 0);

    sqlite3_stmt *s;
    /* Last message: role=user, content=recall text */
    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.messages[#-1].role'),"
        " json_extract(?1,'$.messages[#-1].content')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "user") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), recall) == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    /* Gemini: recall appended as final user content, NOT in systemInstruction */
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(llm_build_payload(db, sid, &cfg, &plan, recall, "You are helpful.", &payload) == 0);

    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.contents[#-1].role'),"
        " json_extract(?1,'$.contents[#-1].parts[0].text'),"
        " json_extract(?1,'$.systemInstruction.parts[0].text')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "user") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), recall) == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "You are helpful.") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_recall_tail_user_message\n");
}

static void test_payload_with_tools(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    /* Seed a tool in the tools table */
    sqlite3_exec(db,
        "INSERT INTO tools(name,description,parameters_json) VALUES("
        "'file_read','Read a file',"
        "'{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}')",
        NULL, NULL, NULL);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.provider.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    assert(payload.body);

    sqlite3_stmt *s;

    /* Check tools array has 1 element */
    sqlite3_prepare_v2(db, "SELECT json_array_length(json_extract(?1,'$.tools'))", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);

    /* Check tool name */
    json_get_str(db, payload.body, "$.tools[0].function.name", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "file_read") == 0);
    sqlite3_finalize(s);

    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_payload_with_tools\n");
}

int main(void) {
    printf("test_llm_payload:\n");
    test_openai_payload();
    test_openai_payload_no_stream_omits_nulls();
    test_gemini_payload();
    test_recall_tail_user_message();
    test_payload_with_tools();
    printf("All payload tests passed.\n");
    return 0;
}
