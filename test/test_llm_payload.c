#include "db.h"
#include "test_util.h"
#include "hook_dispatch.h"
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
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_gemini_payload\n");
}

/* Session context (recall + live state, wrapped in <RELEVANT_CONTEXT>) rides
 * SECOND-TO-LAST — right before the newest real entry — on both endpoints,
 * never in the system prompt / systemInstruction. This ordering lets the
 * model see "here's context, here's what just happened" with the actual
 * new turn as the true tail. */
static void test_recall_in_session_context(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

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
    /* Last message is the real newest entry (assistant reply); second-to-
     * last is the session context block. */
    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.messages[#-1].role'),"
        " json_extract(?1,'$.messages[#-1].content'),"
        " json_extract(?1,'$.messages[#-2].role'),"
        " json_extract(?1,'$.messages[#-2].content')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "assistant") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "Hi there!") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "user") == 0);
    assert(strstr((const char *)sqlite3_column_text(s, 3), recall));
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    /* Gemini: same second-to-last placement, NOT in systemInstruction */
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    assert(llm_build_payload(db, sid, &cfg, &plan, context_text, "You are helpful.", &payload) == 0);

    sqlite3_prepare_v2(db,
        "SELECT json_extract(?1,'$.contents[#-1].role'),"
        " json_extract(?1,'$.contents[#-1].parts[0].text'),"
        " json_extract(?1,'$.contents[#-2].role'),"
        " json_extract(?1,'$.contents[#-2].parts[0].text'),"
        " json_extract(?1,'$.systemInstruction.parts[0].text')", -1, &s, NULL);
    sqlite3_bind_text(s, 1, payload.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "model") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "Hi there!") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "user") == 0);
    assert(strstr((const char *)sqlite3_column_text(s, 3), recall));
    assert(strcmp((const char *)sqlite3_column_text(s, 4), "You are helpful.") == 0);
    sqlite3_finalize(s);
    llm_payload_release(&payload);

    free(context_text);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_recall_in_session_context\n");
}

/* session_context_text: pending approvals and running sub-agents surface in
 * the assembled payload; the block is entirely absent when there's nothing
 * pending/running and no recall. */
static void test_session_context_live_state(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    /* Absent case: nothing pending, no recall */
    char *empty = session_context_text(db, sid, NULL);
    assert(empty == NULL);

    /* Pending approval */
    sqlite3_stmt *ins;
    sqlite3_prepare_v2(db,
        "INSERT INTO approvals(session_id,tool_name,action,state)"
        " VALUES(?1,'grant_host','request_config','pending');",
        -1, &ins, NULL);
    sqlite3_bind_int64(ins, 1, sid);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);

    /* Running sub-agent */
    int64_t child = session_create(db, "worker-session", "worker", sid, 0);
    assert(child > 0);
    assert(session_set_state(db, child, "tool_running") == 0);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    char *context_text = session_context_text(db, sid, NULL);
    assert(context_text);
    assert(strstr(context_text, "<pending_approvals>"));
    assert(strstr(context_text, "grant_host"));
    assert(strstr(context_text, "<running_sub_agents>"));
    assert(strstr(context_text, "worker"));
    assert(!strstr(context_text, "<recall>"));

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, context_text, "You are helpful.", &payload) == 0);
    assert(strstr(payload.body, "grant_host"));
    assert(strstr(payload.body, "worker"));
    llm_payload_release(&payload);

    free(context_text);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_session_context_live_state\n");
}

static void test_payload_with_tools(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

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
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_payload_with_tools\n");
}

/* Compaction summaries must reach the model on BOTH endpoints: as a system
 * message in OpenAI format, and as a user text part in Gemini format (which
 * filters type='system' because the prompt rides in systemInstruction). */
static void test_compaction_entry_in_payload(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "test", "default", -1, 0);
    assert(sid > 0);
    Message m1 = {.role = ROLE_USER, .content = "first question"};
    int64_t e1 = entry_append_with_turn(db, sid, &m1, 1);
    Message m2 = {.role = ROLE_ASSISTANT, .content = "first answer"};
    entry_append_with_turn(db, sid, &m2, 1);
    Message m3 = {.role = ROLE_USER, .content = "next question"};
    entry_append_with_turn(db, sid, &m3, 2);
    Message m4 = {.role = ROLE_ASSISTANT, .content = "next answer"};
    int64_t e4 = entry_append_with_turn(db, sid, &m4, 2);
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

/* A tool_result whose entry carries a network_hosts tag is wrapped in
 * UNTRUSTED_EXTERNAL_CONTENT boundaries at query time; untagged results
 * pass through bare. Both endpoints. */
static void test_network_hosts_query_time_wrap(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "test", "default", -1, 0);
    assert(sid > 0);
    Message m1 = {.role = ROLE_USER, .content = "fetch something"};
    entry_append_with_turn(db, sid, &m1, 1);
    Message asst = {.role = ROLE_ASSISTANT, .content = "fetching"};
    entry_append_with_turn(db, sid, &asst, 1);

    ToolResult tr1 = {.tool_call_id = "call_net", .content = "external page body"};
    Message r1 = {.role = ROLE_TOOL, .tool_result = &tr1, .tool_name = "web_fetch"};
    int64_t rid1 = entry_append_with_turn(db, sid, &r1, 1);
    assert(rid1 > 0);
    assert(db_entry_set_network_hosts(db, rid1, "[\"example.com\"]") == 0);

    ToolResult tr2 = {.tool_call_id = "call_local", .content = "local file body"};
    Message r2 = {.role = ROLE_TOOL, .tool_result = &tr2, .tool_name = "file_read"};
    int64_t rid2 = entry_append_with_turn(db, sid, &r2, 1);
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

    db_close(db); unlink(DB_PATH);
    printf("  PASS test_network_hosts_query_time_wrap\n");
}

/* Ephemeral hook injects ride at the tail of history on both endpoints and
 * vanish once hook_directives_clear runs (llm_req exit semantics). */
static void test_hook_inject_directive(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

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
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_hook_inject_directive\n");
}

int main(void) {
    printf("test_llm_payload:\n");
    test_openai_payload();
    test_openai_payload_no_stream_omits_nulls();
    test_gemini_payload();
    test_recall_in_session_context();
    test_session_context_live_state();
    test_payload_with_tools();
    test_compaction_entry_in_payload();
    test_network_hosts_query_time_wrap();
    test_hook_inject_directive();
    printf("All payload tests passed.\n");
    return 0;
}
