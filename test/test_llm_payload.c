#define _POSIX_C_SOURCE 200809L   /* setenv/unsetenv/tzset under -std=c11 */
#include "db.h"
#include "test_util.h"
#include "hook_dispatch.h"
#include "llm_payload.h"
#include "config.h"
#include "config_registry.h"
#include "context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_payload.db";

/* Clean + open + seed the agents this file writes child rows for (sessions,
 * grants, agent_extensions, memory_blocks) — agent_name is an enforced FK. */
static sqlite3 *open_seeded(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    test_seed_agent(db, "default");
    test_seed_agent(db, "worker");
    return db;
}

static void setup_session(sqlite3 *db, int64_t *sid) {
    *sid = session_create(db, "test", "default", -1, 0);
    assert(*sid > 0);

    /* User message with special chars */
    Message user = {.role = ROLE_USER, .content = "Hello \"world\"\nLine2\ttab"};
    entry_append_with_iteration(db, *sid, &user, 1);

    /* Assistant reply */
    Message asst = {.role = ROLE_ASSISTANT, .content = "Hi there!"};
    entry_append_with_iteration(db, *sid, &asst, 1);
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
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

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

    /* Stream key must never be emitted (streaming removed) */
    sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.stream')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_type(s, 0) == SQLITE_NULL);
    sqlite3_finalize(s);

    /* Check max_tokens */
    sqlite3_prepare_v2(db, "SELECT json_extract(?1,'$.max_tokens')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1024);
    sqlite3_finalize(s);

    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_openai_payload\n");
}

/* Regression: stream=0 / max_tokens=0 / no tools must OMIT the keys entirely.
 * json_object emits JSON null for SQL NULL; strict providers (DeepSeek) 400
 * on "stream":null. */
static void test_openai_payload_no_stream_omits_nulls(void) {
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 0;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

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
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_openai_payload_no_stream_omits_nulls\n");
}

static void test_gemini_payload(void) {
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    Config cfg = {0};
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.max_tokens = 2048;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.context_window = 128000;

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
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_gemini_payload\n");
}

/* Session context (recall + live state, wrapped in <RELEVANT_CONTEXT>) rides
 * at the TURN BOUNDARY — right before the newest user_message — on both
 * endpoints, never in the system prompt / systemInstruction. The user's
 * actual message stays the true tail of its turn, and the block's position
 * never moves as tool-loop iterations append entries after it. */
static void test_recall_in_session_context(void) {
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    const char *recall = "---Possibly relevant context---\n[session 3, user] hi\n---End of context---";

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    char *context_text = session_context_text(db, sid, recall);
    assert(context_text);
    assert(strstr(context_text, "<RELEVANT_CONTEXT>"));
    assert(strstr(context_text, "<recall>"));
    assert(strstr(context_text, recall));
    assert(strstr(context_text, "</recall>"));
    assert(strstr(context_text, "</RELEVANT_CONTEXT>"));

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, context_text, "You are helpful.", &payload) == 0);

    sqlite3_stmt *s;
    /* Tail is the real conversation (user then assistant); the context
     * block rides right BEFORE the newest user message. */
    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.messages[#-1].role'),"
        " json_extract(?1,'$.messages[#-1].content'),"
        " json_extract(?1,'$.messages[#-2].role'),"
        " json_extract(?1,'$.messages[#-2].content'),"
        " json_extract(?1,'$.messages[#-3].role'),"
        " json_extract(?1,'$.messages[#-3].content')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "assistant") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "Hi there!") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "user") == 0);
    assert(strstr((const char *)sqlite3_column_text(s, 3), "Hello \"world\""));
    assert(strcmp((const char *)sqlite3_column_text(s, 4), "user") == 0);
    assert(strstr((const char *)sqlite3_column_text(s, 5), recall));
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    /* Gemini: same turn-boundary placement, NOT in systemInstruction */
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(llm_build_payload(db, sid, &cfg, &plan, context_text, "You are helpful.", &payload) == 0);

    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.contents[#-1].role'),"
        " json_extract(?1,'$.contents[#-1].parts[0].text'),"
        " json_extract(?1,'$.contents[#-2].parts[0].text'),"
        " json_extract(?1,'$.contents[#-3].role'),"
        " json_extract(?1,'$.contents[#-3].parts[0].text'),"
        " json_extract(?1,'$.systemInstruction.parts[0].text')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "model") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "Hi there!") == 0);
    assert(strstr((const char *)sqlite3_column_text(s, 2), "Hello \"world\""));
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "user") == 0);
    assert(strstr((const char *)sqlite3_column_text(s, 4), recall));
    assert(strcmp((const char *)sqlite3_column_text(s, 5), "You are helpful.") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    free(context_text);
    context_plan_free(&plan);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_recall_in_session_context\n");
}

/* session_context_text: pending approvals and running sub-agents surface in
 * the assembled payload; with nothing pending/running and no recall the block
 * carries the wall clock alone — the zone is stated, since cron expressions
 * evaluate in local time and the model has to translate "9am" into one. */
static void test_session_context_live_state(void) {
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    /* Quiet case: clock only, with its zone */
    setenv("TZ", "EST5EDT,M3.2.0/2,M11.1.0/2", 1);
    tzset();
    char *quiet = session_context_text(db, sid, NULL);
    assert(quiet);
    assert(strstr(quiet, "<current_time>"));
    assert(strstr(quiet, "EST") || strstr(quiet, "EDT"));
    assert(strstr(quiet, "(UTC-05") || strstr(quiet, "(UTC-04"));
    assert(!strstr(quiet, "<recall>"));
    assert(!strstr(quiet, "<memory_blocks>"));
    free(quiet);
    unsetenv("TZ");
    tzset();

    /* Pending approval */
    sqlite3_stmt *ins;
    sqlite3_prepare_v2(db,
        "INSERT INTO approvals(session_id,tool_name,action,state)"
        " VALUES(?1,'request_config','request_changes','pending');",
        -1, &ins, NULL);
    sqlite3_bind_int64(ins, 1, sid);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);

    /* Running sub-agent */
    int64_t child = session_create(db, "worker-session", "worker", sid, 0);
    assert(child > 0);
    assert(session_set_state(db, child, "tool_running") == 0);

    /* Memory block created with NULL placement — defaults to 'context',
     * so it surfaces here rather than in the system prompt */
    assert(memory_block_create(db, "default", "scratch", "working notes", 5000, NULL) > 0);
    assert(memory_entry_add(db, "default", "scratch", "remember the milk") >= 1);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    char *context_text = session_context_text(db, sid, NULL);
    assert(context_text);
    assert(strstr(context_text, "<pending_approvals>"));
    /* the renderer shows COALESCE(tool_name, action) — tool_name wins */
    assert(strstr(context_text, "request_config"));
    assert(strstr(context_text, "<running_sub_agents>"));
    assert(strstr(context_text, "worker"));
    assert(strstr(context_text, "<memory_blocks>"));
    assert(strstr(context_text, "remember the milk"));
    assert(!strstr(context_text, "<recall>"));

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, context_text, "You are helpful.", &payload) == 0);
    assert(strstr(payload.body, "request_config"));
    assert(strstr(payload.body, "worker"));
    llm_payload_release(&payload);

    free(context_text);
    context_plan_free(&plan);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_session_context_live_state\n");
}

/* Turn-boundary placement under a tool loop: the block must never split an
 * assistant tool_calls message from its tool result (strict providers 400
 * on non-adjacency), and its index must not move as iterations append
 * entries — that position stability is what lets all iterations of a turn
 * share one prompt-cache prefix. Also covers the per-turn freeze store
 * (session_set/get_turn_context). */
static void test_session_context_tool_loop(void) {
    sqlite3 *db = open_seeded();

    int64_t sid = session_create(db, "test", "default", -1, 0);
    assert(sid > 0);

    /* Frozen-per-turn store round-trip */
    assert(session_get_turn_context(db, sid) == NULL);
    assert(session_set_turn_context(db, sid, "frozen") == 0);
    char *tc = session_get_turn_context(db, sid);
    assert(tc && strcmp(tc, "frozen") == 0);
    free(tc);
    assert(session_set_turn_context(db, sid, NULL) == 0);
    assert(session_get_turn_context(db, sid) == NULL);

    const char *ctx = "<RELEVANT_CONTEXT>sentinel</RELEVANT_CONTEXT>";
    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    /* Iteration 0: just the user message. Block lands right before it. */
    assert(entry_append_typed(db, sid, 1, "user_message", 0, "list files",
                              NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0) > 0);

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, ctx, "sys", &payload) == 0);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.messages[1].content'),"
        " json_extract(?1,'$.messages[#-1].role'),"
        " json_extract(?1,'$.messages[#-1].content')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), ctx) == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "user") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "list files") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    /* Iteration 1: assistant tool_calls + tool result appended. The block
     * keeps index 1 (position stable) and the tool message still directly
     * follows its assistant tool_calls message (adjacency preserved). */
    assert(entry_append_typed(db, sid, 1, "assistant_message", 0, "Sure.",
                              NULL, NULL, 0, STOP_REASON_TOOL_USE, NULL, 0, 0, 0) > 0);
    assert(entry_append_typed(db, sid, 1, "tool_call", 1, "{\"cmd\":\"ls\"}",
                              "call_1", "shell_exec", 0, STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
    assert(entry_append_typed(db, sid, 1, "tool_result", 0, "file1\nfile2",
                              "call_1", "shell_exec", 0, STOP_REASON_NONE, NULL, 0, 0, 0) > 0);

    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, ctx, "sys", &payload) == 0);

    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.messages[1].content'),"
        " json_extract(?1,'$.messages[#-1].role'),"
        " json_extract(?1,'$.messages[#-2].role'),"
        " json_extract(?1,'$.messages[#-2].tool_calls[0].id'),"
        " json_extract(?1,'$.messages[#-3].role'),"
        " json_extract(?1,'$.messages[#-3].content')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), ctx) == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "tool") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "assistant") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "call_1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 4), "user") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 5), "list files") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    /* Gemini: functionResponse still directly follows the functionCall
     * turn; the block leads the contents, before the user message. */
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(llm_build_payload(db, sid, &cfg, &plan, ctx, "sys", &payload) == 0);

    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.contents[0].parts[0].text'),"
        " json_extract(?1,'$.contents[1].parts[0].text'),"
        " json_extract(?1,'$.contents[#-1].parts[0].functionResponse.name'),"
        " json_extract(?1,'$.contents[#-2].role'),"
        " json_extract(?1,'$.contents[#-2].parts[1].functionCall.name')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), ctx) == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "list files") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "shell_exec") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "model") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 4), "shell_exec") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_session_context_tool_loop\n");
}

static void test_payload_with_tools(void) {
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    /* Seed a tool in the tools table + a grant — an agent's payload shows
     * only granted tools (zero grants = zero tools) */
    sqlite3_exec(db,
        "INSERT INTO tools(name,description,parameters_json) VALUES("
        "'file_read','Read a file',"
        "'{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}');"
        "INSERT INTO grants(agent_name,kind,value) VALUES('default','tool','file_read');",
        NULL, NULL, NULL);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

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
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_payload_with_tools\n");
}

/* Compaction summaries must reach the model on BOTH endpoints: as a system
 * message in OpenAI format, and as a user text part in Gemini format (which
 * filters type='system' because the prompt rides in systemInstruction). */
static void test_compaction_entry_in_payload(void) {
    sqlite3 *db = open_seeded();

    int64_t sid = session_create(db, "test", "default", -1, 0);
    assert(sid > 0);
    Message m1 = {.role = ROLE_USER, .content = "first question"};
    int64_t e1 = entry_append_with_iteration(db, sid, &m1, 1);
    Message m2 = {.role = ROLE_ASSISTANT, .content = "first answer"};
    entry_append_with_iteration(db, sid, &m2, 1);
    Message m3 = {.role = ROLE_USER, .content = "next question"};
    entry_append_with_iteration(db, sid, &m3, 2);
    Message m4 = {.role = ROLE_ASSISTANT, .content = "next answer"};
    int64_t e4 = entry_append_with_iteration(db, sid, &m4, 2);
    assert(e1 > 0 && e4 > 0);
    /* Summarize e2+e3 away; keep e1 and e4 */
    int64_t cid = entry_compact(db, sid, e1, e4,
                                "Earlier: user greeted, assistant replied.");
    assert(cid > 0);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    assert(strstr(payload.body, "Earlier: user greeted") != NULL);
    /* rendered as system, not user */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM json_each(json_extract(?1,'$.messages'))"
        " WHERE json_extract(value,'$.role')='system'"
        "   AND json_extract(value,'$.content') LIKE '%Earlier: user greeted%'",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    /* Gemini: summary must survive the type!='system' filter as user text */
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    assert(strstr(payload.body, "Earlier: user greeted") != NULL);
    assert(strstr(payload.body, "[Summary of earlier conversation]") != NULL);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    db_close(db);
    printf("  PASS test_compaction_entry_in_payload\n");
}

/* A cron_result rides the assistant role so ordinary delivery finds it, which
 * is exactly why the serializer has to label it: unlabelled, the model reads a
 * scheduled script's stdout back as its own words. Both endpoints — and the
 * entry must reach the payload at all (an unknown type serializes to NULL and
 * is dropped silently). */
static void test_cron_result_in_payload(void) {
    sqlite3 *db = open_seeded();

    int64_t sid = session_create(db, "test", "default", -1, 0);
    assert(sid > 0);
    Message m1 = {.role = ROLE_USER, .content = "hello"};
    entry_append_with_iteration(db, sid, &m1, 1);
    Message m2 = {.role = ROLE_ASSISTANT, .content = "hi"};
    entry_append_with_iteration(db, sid, &m2, 1);
    assert(entry_append_cron_result(db, sid, "disk_watch", "42% used", 0) > 0);
    assert(entry_append_cron_result(db, sid, "net_probe", "timed out", 1) > 0);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);
    assert(strstr(payload.body, "[scheduled script disk_watch output]") != NULL);
    assert(strstr(payload.body, "42% used") != NULL);
    assert(strstr(payload.body, "[scheduled script net_probe failed]") != NULL);
    /* assistant role, so a strict provider still sees a legal alternation */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM json_each(json_extract(?1,'$.messages'))"
        " WHERE json_extract(value,'$.role')='assistant'"
        "   AND json_extract(value,'$.content') LIKE '%scheduled script%'",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 2);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);
    assert(strstr(payload.body, "[scheduled script disk_watch output]") != NULL);
    assert(strstr(payload.body, "42% used") != NULL);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    db_close(db);
    printf("  PASS test_cron_result_in_payload\n");
}

/* A tool_result whose entry carries a network_hosts tag is wrapped in
 * UNTRUSTED_EXTERNAL_CONTENT boundaries at query time; untagged results
 * pass through bare. Both endpoints. */
static void test_network_hosts_query_time_wrap(void) {
    sqlite3 *db = open_seeded();

    int64_t sid = session_create(db, "test", "default", -1, 0);
    assert(sid > 0);
    Message m1 = {.role = ROLE_USER, .content = "fetch something"};
    entry_append_with_iteration(db, sid, &m1, 1);
    Message asst = {.role = ROLE_ASSISTANT, .content = "fetching"};
    entry_append_with_iteration(db, sid, &asst, 1);

    ToolResult tr1 = {.tool_call_id = "call_net", .content = "external page body"};
    Message r1 = {.role = ROLE_TOOL, .tool_result = &tr1, .tool_name = "web_fetch"};
    int64_t rid1 = entry_append_with_iteration(db, sid, &r1, 1);
    assert(rid1 > 0);
    assert(db_entry_set_network_hosts(db, rid1, "[\"example.com\"]") == 0);

    ToolResult tr2 = {.tool_call_id = "call_local", .content = "local file body"};
    Message r2 = {.role = ROLE_TOOL, .tool_result = &tr2, .tool_name = "file_read"};
    int64_t rid2 = entry_append_with_iteration(db, sid, &r2, 1);
    assert(rid2 > 0);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);
    assert(strstr(payload.body, "<<<UNTRUSTED_EXTERNAL_CONTENT>>>") != NULL);
    assert(strstr(payload.body, "<<<END_UNTRUSTED_EXTERNAL_CONTENT>>>") != NULL);
    assert(strstr(payload.body, "Contacted hosts: [\\\"example.com\\\"]") != NULL);
    assert(strstr(payload.body, "external page body") != NULL);
    /* the untagged result is NOT wrapped: exactly one wrapped message */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM json_each(json_extract(?1,'$.messages'))"
        " WHERE json_extract(value,'$.role')='tool'"
        "   AND json_extract(value,'$.content') LIKE '%UNTRUSTED_EXTERNAL_CONTENT%'",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);
    sqlite3_prepare_v2(db,
        "SELECT json_extract(value,'$.content')"
        " FROM json_each(json_extract(?1,'$.messages'))"
        " WHERE json_extract(value,'$.tool_call_id')='call_local'",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "local file body") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    /* Gemini: same wrap inside functionResponse content */
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);
    assert(strstr(payload.body, "<<<UNTRUSTED_EXTERNAL_CONTENT>>>") != NULL);
    assert(strstr(payload.body, "external page body") != NULL);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_network_hosts_query_time_wrap\n");
}

/* Ephemeral hook injects ride at the tail of history on both endpoints and
 * vanish once hook_directives_clear runs (llm_req exit semantics). */
static void test_hook_inject_directive(void) {
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    sqlite3_stmt *ins;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO hook_directives(session_id, kind, role, content)"
        " VALUES(?1,'inject','system','User timezone: America/Chicago');",
        -1, &ins, NULL) == SQLITE_OK);
    sqlite3_bind_int64(ins, 1, sid);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    /* OpenAI: inject is the LAST message (after all history entries) */
    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.messages[#-1].role'),"
        " json_extract(?1,'$.messages[#-1].content')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "system") == 0);
    assert(strstr((const char *)sqlite3_column_text(s, 1), "America/Chicago"));
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    /* Gemini: system inject becomes a '[system] '-prefixed user part at tail */
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.contents[#-1].role'),"
        " json_extract(?1,'$.contents[#-1].parts[0].text')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "user") == 0);
    assert(strncmp((const char *)sqlite3_column_text(s, 1), "[system] ", 9) == 0);
    assert(strstr((const char *)sqlite3_column_text(s, 1), "America/Chicago"));
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    /* Cleared directives (llm_req exit) leave the next payload inject-free */
    hook_directives_clear(db, sid);
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM json_each(json_extract(?1,'$.messages'))"
        " WHERE json_extract(value,'$.content') LIKE '%America/Chicago%'", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    context_plan_free(&plan);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_hook_inject_directive\n");
}

/* An extension tool attached via agent_extensions but NOT present in the
 * agent's grants must NOT appear in the payload tools array; once its name
 * IS in the granted list, it DOES appear. This exercises the SQL_OPENAI_TOOLS
 * grant-only filter (no attachment OR-branch). */
static void test_extension_tool_grant_filtering(void) {
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    /* Seed an extension tool that the session's agent ('default') has attached
     * but NOT yet granted. Also seed a builtin tool that IS granted (control). */
    sqlite3_exec(db,
        "INSERT INTO extensions(name, path, owner_agent, published, enabled)"
        " VALUES('nws','/tmp/ext/nws','default',1,1);"
        "INSERT INTO agent_extensions(agent_name, extension_name, enabled)"
        " VALUES('default','nws',1);"
        "INSERT INTO tools(name, extension_name, description, parameters_json, path, enabled)"
        " VALUES('get_forecast','nws','Get forecast',"
        "'{\"type\":\"object\",\"properties\":{\"lat\":{\"type\":\"number\"}}}','/tmp/ext/nws/forecast.qjs',1);"
        "INSERT INTO tools(name, description, parameters_json, enabled)"
        " VALUES('file_read','Read a file',"
        "'{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}',1);"
        "INSERT INTO grants(agent_name,kind,value) VALUES('default','tool','file_read');",
        NULL, NULL, NULL);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    /* Build payload WITHOUT the extension tool granted. */
    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);

    sqlite3_stmt *s;
    /* Only the granted builtin should appear (count=1). */
    sqlite3_prepare_v2(db, "SELECT json_array_length(json_extract(?1,'$.tools'))", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);

    /* The one tool must be file_read, not get_forecast. */
    json_get_str(db, payload.body, "$.tools[0].function.name", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "file_read") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    /* Now grant the extension tool to 'default'. */
    sqlite3_exec(db,
        "INSERT INTO grants(agent_name,kind,value) VALUES('default','tool','get_forecast');",
        NULL, NULL, NULL);

    /* Rebuild payload — both tools should now appear. */
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);

    sqlite3_prepare_v2(db, "SELECT json_array_length(json_extract(?1,'$.tools'))", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 2);
    sqlite3_finalize(s);

    /* Verify get_forecast is among them. */
    sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM json_each(json_extract(?1,'$.tools'))"
        " WHERE json_extract(value,'$.function.name')='get_forecast'",
        -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);

    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_extension_tool_grant_filtering\n");
}

/* launch_agent's description is assembled at payload-build time: the live
 * agent roster AND the live default worker toolset. The toolset matters
 * because the tool schema can only *name* the worker_tools config, which a
 * sandboxed worker cannot read — the spawner has to be able to see the list
 * to know what its worker will be missing. Other tools' descriptions are
 * passed through untouched. */
static void test_launch_agent_description_embeds_roster_and_worker_tools(void) {
    sqlite3 *db = open_seeded();

    int64_t sid;
    setup_session(db, &sid);

    sqlite3_exec(db,
        "UPDATE agents SET description='Watches the feed' WHERE name='worker';"
        "INSERT INTO tools(name,description,parameters_json) VALUES("
        "'launch_agent','Delegate a task.','{\"type\":\"object\"}');"
        "INSERT INTO tools(name,description,parameters_json) VALUES("
        "'file_read','Read a file.','{\"type\":\"object\"}');"
        "INSERT INTO grants(agent_name,kind,value) VALUES('default','tool','launch_agent');"
        "INSERT INTO grants(agent_name,kind,value) VALUES('default','tool','file_read');",
        NULL, NULL, NULL);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT json_extract(value,'$.function.description')"
        " FROM json_each(json_extract(?1,'$.tools'))"
        " WHERE json_extract(value,'$.function.name')=?2", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, "launch_agent", -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *d = (const char *)sqlite3_column_text(s, 0);
    assert(d && strncmp(d, "Delegate a task.", 16) == 0);
    /* roster (pre-existing behaviour) */
    assert(strstr(d, "Available agents:"));
    assert(strstr(d, "worker — Watches the feed"));
    /* worker toolset, spelled out, in registry order */
    assert(strstr(d, "Default worker toolset (self-spawn with no 'tools'): "
                     "file_read, file_write, shell_exec, web_fetch, js_eval, "
                     "launch_agent, check_session, search_config, secret_create."));
    sqlite3_finalize(s);

    /* Any other tool's description is untouched — no roster, no toolset. */
    sqlite3_prepare_v2(db,
        "SELECT json_extract(value,'$.function.description')"
        " FROM json_each(json_extract(?1,'$.tools'))"
        " WHERE json_extract(value,'$.function.name')=?2", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, "file_read", -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "Read a file.") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    /* Live, not baked in: an operator override shows up on the next request. */
    assert(config_set(db, "worker_tools", "[\"js_eval\"]") == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);
    assert(strstr(payload.body, "Default worker toolset (self-spawn with no 'tools'): js_eval."));
    llm_payload_release(&payload);

    /* A malformed override drops the sentence rather than failing the build. */
    assert(config_set(db, "worker_tools", "not json") == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);
    assert(strstr(payload.body, "Available agents:"));
    assert(!strstr(payload.body, "Default worker toolset"));
    llm_payload_release(&payload);

    /* Gemini carries the same assembled description. */
    assert(config_set(db, "worker_tools", "[\"js_eval\"]") == 0);
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "sys", &payload) == 0);
    assert(strstr(payload.body, "Available agents:"));
    assert(strstr(payload.body, "Default worker toolset (self-spawn with no 'tools'): js_eval."));
    llm_payload_release(&payload);

    context_plan_free(&plan);
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_launch_agent_description_embeds_roster_and_worker_tools\n");
}

/* A route-pinned session gets its route's system_prompt_suffix appended to
 * the system prompt, on both endpoint shapes; an unpinned session (positive
 * control) sees the prompt unchanged. */
static void test_route_prompt_suffix(void) {
    sqlite3 *db = open_seeded();

    int64_t sid, plain;
    setup_session(db, &sid);
    setup_session(db, &plain);

    sqlite3_stmt *ins;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO channel_routes(channel_name, chat_id, session_id,"
        "                           system_prompt_suffix)"
        " VALUES('discord','chan1',?1,'House rules: keep it short.');",
        -1, &ins, NULL) == SQLITE_OK);
    sqlite3_bind_int64(ins, 1, sid);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    LlmPayload payload;
    sqlite3_stmt *s;

    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    json_get_str(db, payload.body, "$.messages[0].content", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "You are helpful.\n\nHouse rules: keep it short.") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    /* Positive control: no route, no suffix, byte-identical prompt. */
    assert(context_plan(db, plain, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, plain, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    json_get_str(db, payload.body, "$.messages[0].content", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "You are helpful.") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    /* Gemini carries the system prompt in systemInstruction. */
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    json_get_str(db, payload.body, "$.systemInstruction.parts[0].text", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "You are helpful.\n\nHouse rules: keep it short.") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_route_prompt_suffix\n");
}

/* A channel-bound session is told where it is chatting: chat_title when the
 * runner reported one, channel+chat_id otherwise. The route suffix, if any,
 * follows the location line. */
static void test_chat_location_line(void) {
    sqlite3 *db = open_seeded();

    int64_t named, unnamed;
    setup_session(db, &named);
    setup_session(db, &unnamed);

    char sql[512];
    snprintf(sql, sizeof(sql),
        "UPDATE sessions SET channel_name='discord', chat_id='chan1',"
        " chat_title='#general @ My Server' WHERE id=%lld;"
        "UPDATE sessions SET channel_name='discord', chat_id='chan2'"
        " WHERE id=%lld;"
        "INSERT INTO channel_routes(channel_name, chat_id, session_id,"
        "                           system_prompt_suffix)"
        " VALUES('discord','chan1',%lld,'House rules: keep it short.');",
        (long long)named, (long long)unnamed, (long long)named);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    LlmPayload payload;
    sqlite3_stmt *s;

    assert(context_plan(db, named, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, named, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    json_get_str(db, payload.body, "$.messages[0].content", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "You are helpful.\n\nYou are chatting in #general @ My Server"
                  " (channel discord, chat id chan1).\n\n"
                  "House rules: keep it short.") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    /* No observed name yet — the ids still say where we are. */
    assert(context_plan(db, unnamed, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, unnamed, &cfg, &plan, NULL, "You are helpful.", &payload) == 0);
    json_get_str(db, payload.body, "$.messages[0].content", &s);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "You are helpful.\n\nYou are chatting in chat chan2"
                  " (channel discord, chat id chan2).") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);
    context_plan_free(&plan);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_chat_location_line\n");
}

int main(void) {
    TEST_INIT();
    printf("test_llm_payload:\n");
    test_openai_payload();
    test_openai_payload_no_stream_omits_nulls();
    test_gemini_payload();
    test_recall_in_session_context();
    test_session_context_live_state();
    test_session_context_tool_loop();
    test_payload_with_tools();
    test_compaction_entry_in_payload();
    test_cron_result_in_payload();
    test_network_hosts_query_time_wrap();
    test_hook_inject_directive();
    test_extension_tool_grant_filtering();
    test_launch_agent_description_embeds_roster_and_worker_tools();
    test_route_prompt_suffix();
    test_chat_location_line();
    printf("All payload tests passed.\n");
    return 0;
}
