#define _POSIX_C_SOURCE 200809L
#include "db_request.h"
#include "db.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *test_db(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);
    return db;
}

/* Test: basic OpenAI request with user message */
static void test_openai_basic(void) {
    printf("  test_openai_basic...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    Message msg = {.role = ROLE_USER, .content = "hello"};
    int64_t eid = entry_append(db, sid, &msg);
    assert(eid > 0);

    int64_t ids[] = {eid};
    char *json = db_build_request(db, sid, ids, 1, "gpt-4", ENDPOINT_OPENAI,
                                  NULL, 4096, 0, -1);
    assert(json);

    cJSON *root = cJSON_Parse(json);
    assert(root);
    assert(strcmp(cJSON_GetObjectItem(root, "model")->valuestring, "gpt-4") == 0);
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    assert(messages && cJSON_GetArraySize(messages) == 1);
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    assert(strcmp(cJSON_GetObjectItem(m0, "role")->valuestring, "user") == 0);
    assert(strcmp(cJSON_GetObjectItem(m0, "content")->valuestring, "hello") == 0);
    assert(!cJSON_GetObjectItem(root, "tools"));
    assert(!cJSON_GetObjectItem(root, "stream"));
    cJSON *mt = cJSON_GetObjectItem(root, "max_tokens");
    assert(mt && mt->valueint == 4096);

    cJSON_Delete(root);
    free(json);
    db_close(db);
    printf(" OK\n");
}

/* Test: OpenAI with system + user + assistant + tool_calls + tool_result */
static void test_openai_tool_calls(void) {
    printf("  test_openai_tool_calls...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    int64_t e1 = entry_append(db, sid, &sys);

    Message usr = {.role = ROLE_USER, .content = "run ls"};
    int64_t e2 = entry_append(db, sid, &usr);

    /* Assistant with tool_calls */
    ToolCall tc = {.id = "call_1", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}"};
    Message asst = {.role = ROLE_ASSISTANT, .content = NULL,
                    .tool_calls = &tc, .tool_call_count = 1,
                    .stop_reason = STOP_REASON_TOOL_USE};
    int64_t e3 = entry_append(db, sid, &asst);

    /* Tool result */
    ToolResult tr = {.tool_call_id = "call_1", .content = "file1.txt\nfile2.txt"};
    Message tool = {.role = ROLE_TOOL, .tool_result = &tr, .tool_name = "shell_exec"};
    int64_t e4 = entry_append(db, sid, &tool);

    int64_t ids[] = {e1, e2, e3, e4};
    char *json = db_build_request(db, sid, ids, 4, "deepseek-chat", ENDPOINT_OPENAI,
                                  NULL, 0, 1, -1);
    assert(json);

    cJSON *root = cJSON_Parse(json);
    assert(root);
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    assert(cJSON_GetArraySize(messages) == 4);

    /* Check system */
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    assert(strcmp(cJSON_GetObjectItem(m0, "role")->valuestring, "system") == 0);

    /* Check user */
    cJSON *m1 = cJSON_GetArrayItem(messages, 1);
    assert(strcmp(cJSON_GetObjectItem(m1, "role")->valuestring, "user") == 0);

    /* Check assistant with tool_calls */
    cJSON *m2 = cJSON_GetArrayItem(messages, 2);
    assert(strcmp(cJSON_GetObjectItem(m2, "role")->valuestring, "assistant") == 0);
    cJSON *tcs = cJSON_GetObjectItem(m2, "tool_calls");
    assert(tcs && cJSON_GetArraySize(tcs) == 1);
    cJSON *tc0 = cJSON_GetArrayItem(tcs, 0);
    assert(strcmp(cJSON_GetObjectItem(tc0, "id")->valuestring, "call_1") == 0);
    assert(strcmp(cJSON_GetObjectItem(tc0, "type")->valuestring, "function") == 0);
    cJSON *fn = cJSON_GetObjectItem(tc0, "function");
    assert(strcmp(cJSON_GetObjectItem(fn, "name")->valuestring, "shell_exec") == 0);
    /* arguments should be a string (stringified JSON) */
    cJSON *args = cJSON_GetObjectItem(fn, "arguments");
    assert(args && cJSON_IsString(args));
    assert(strstr(args->valuestring, "cmd") != NULL);

    /* Check tool result */
    cJSON *m3 = cJSON_GetArrayItem(messages, 3);
    assert(strcmp(cJSON_GetObjectItem(m3, "role")->valuestring, "tool") == 0);
    assert(strcmp(cJSON_GetObjectItem(m3, "tool_call_id")->valuestring, "call_1") == 0);

    /* stream should be true */
    assert(cJSON_IsTrue(cJSON_GetObjectItem(root, "stream")));
    /* no max_tokens (was 0) */
    assert(!cJSON_GetObjectItem(root, "max_tokens"));

    cJSON_Delete(root);
    free(json);
    db_close(db);
    printf(" OK\n");
}

/* Test: Gemini format */
static void test_gemini_basic(void) {
    printf("  test_gemini_basic...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys = {.role = ROLE_SYSTEM, .content = "Be concise."};
    int64_t e1 = entry_append(db, sid, &sys);

    Message usr = {.role = ROLE_USER, .content = "hi"};
    int64_t e2 = entry_append(db, sid, &usr);

    Message asst = {.role = ROLE_ASSISTANT, .content = "hello",
                    .stop_reason = STOP_REASON_STOP};
    int64_t e3 = entry_append(db, sid, &asst);

    int64_t ids[] = {e1, e2, e3};
    char *json = db_build_request(db, sid, ids, 3, "gemini-2.5-flash", ENDPOINT_GEMINI,
                                  NULL, 8192, 0, -1);
    assert(json);

    cJSON *root = cJSON_Parse(json);
    assert(root);

    /* systemInstruction present */
    cJSON *si = cJSON_GetObjectItem(root, "systemInstruction");
    assert(si);
    cJSON *parts = cJSON_GetObjectItem(si, "parts");
    assert(parts && cJSON_GetArraySize(parts) == 1);
    cJSON *p0 = cJSON_GetArrayItem(parts, 0);
    assert(strcmp(cJSON_GetObjectItem(p0, "text")->valuestring, "Be concise.") == 0);

    /* contents should have 2 entries (user + model), system excluded */
    cJSON *contents = cJSON_GetObjectItem(root, "contents");
    assert(contents && cJSON_GetArraySize(contents) == 2);

    cJSON *c0 = cJSON_GetArrayItem(contents, 0);
    assert(strcmp(cJSON_GetObjectItem(c0, "role")->valuestring, "user") == 0);
    cJSON *c1 = cJSON_GetArrayItem(contents, 1);
    assert(strcmp(cJSON_GetObjectItem(c1, "role")->valuestring, "model") == 0);

    /* generationConfig */
    cJSON *gc = cJSON_GetObjectItem(root, "generationConfig");
    assert(gc);
    assert(cJSON_GetObjectItem(gc, "maxOutputTokens")->valueint == 8192);

    cJSON_Delete(root);
    free(json);
    db_close(db);
    printf(" OK\n");
}

/* Test: Gemini with tool_calls (functionCall parts) */
static void test_gemini_tool_calls(void) {
    printf("  test_gemini_tool_calls...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message usr = {.role = ROLE_USER, .content = "list files"};
    int64_t e1 = entry_append(db, sid, &usr);

    ToolCall tc = {.id = "tc1", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}"};
    Message asst = {.role = ROLE_ASSISTANT, .content = "I'll run ls",
                    .tool_calls = &tc, .tool_call_count = 1,
                    .stop_reason = STOP_REASON_TOOL_USE};
    int64_t e2 = entry_append(db, sid, &asst);

    ToolResult tr = {.tool_call_id = "tc1", .content = "a.txt"};
    Message tool = {.role = ROLE_TOOL, .tool_result = &tr, .tool_name = "shell_exec"};
    int64_t e3 = entry_append(db, sid, &tool);

    int64_t ids[] = {e1, e2, e3};
    char *json = db_build_request(db, sid, ids, 3, "gemini-2.5-flash", ENDPOINT_GEMINI,
                                  NULL, 0, 0, -1);
    assert(json);

    cJSON *root = cJSON_Parse(json);
    assert(root);
    cJSON *contents = cJSON_GetObjectItem(root, "contents");
    assert(contents && cJSON_GetArraySize(contents) == 3);

    /* model with text + functionCall */
    cJSON *c1 = cJSON_GetArrayItem(contents, 1);
    assert(strcmp(cJSON_GetObjectItem(c1, "role")->valuestring, "model") == 0);
    cJSON *parts = cJSON_GetObjectItem(c1, "parts");
    assert(parts && cJSON_GetArraySize(parts) == 2);
    /* first part: text */
    cJSON *p0 = cJSON_GetArrayItem(parts, 0);
    assert(cJSON_GetObjectItem(p0, "text"));
    /* second part: functionCall */
    cJSON *p1 = cJSON_GetArrayItem(parts, 1);
    cJSON *fc = cJSON_GetObjectItem(p1, "functionCall");
    assert(fc);
    assert(strcmp(cJSON_GetObjectItem(fc, "name")->valuestring, "shell_exec") == 0);
    /* args should be an object (not stringified) */
    cJSON *args = cJSON_GetObjectItem(fc, "args");
    assert(args && cJSON_IsObject(args));
    assert(strcmp(cJSON_GetObjectItem(args, "cmd")->valuestring, "ls") == 0);

    /* tool result → functionResponse in user message */
    cJSON *c2 = cJSON_GetArrayItem(contents, 2);
    assert(strcmp(cJSON_GetObjectItem(c2, "role")->valuestring, "user") == 0);
    cJSON *parts2 = cJSON_GetObjectItem(c2, "parts");
    cJSON *fr = cJSON_GetObjectItem(cJSON_GetArrayItem(parts2, 0), "functionResponse");
    assert(fr);
    assert(strcmp(cJSON_GetObjectItem(fr, "name")->valuestring, "tc1") == 0);

    cJSON_Delete(root);
    free(json);
    db_close(db);
    printf(" OK\n");
}

/* Test: cache_break_after marks system messages for cache_control */
static void test_openai_cache_break(void) {
    printf("  test_openai_cache_break...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys1 = {.role = ROLE_SYSTEM, .content = "base prompt"};
    int64_t e1 = entry_append(db, sid, &sys1);

    Message sys2 = {.role = ROLE_SYSTEM, .content = "extra context"};
    int64_t e2 = entry_append(db, sid, &sys2);

    Message usr = {.role = ROLE_USER, .content = "go"};
    int64_t e3 = entry_append(db, sid, &usr);

    /* cache_break_after = e1: sys2 (id > e1) should get cache_control */
    int64_t ids[] = {e1, e2, e3};
    char *json = db_build_request(db, sid, ids, 3, "test", ENDPOINT_OPENAI,
                                  NULL, 0, 0, e1);
    assert(json);

    cJSON *root = cJSON_Parse(json);
    assert(root);
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    assert(cJSON_GetArraySize(messages) == 3);

    /* First system: plain string content (id == cache_break, not >) */
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    cJSON *c0 = cJSON_GetObjectItem(m0, "content");
    assert(cJSON_IsString(c0));

    /* Second system: content is array with cache_control */
    cJSON *m1 = cJSON_GetArrayItem(messages, 1);
    cJSON *c1 = cJSON_GetObjectItem(m1, "content");
    assert(cJSON_IsArray(c1));
    cJSON *block = cJSON_GetArrayItem(c1, 0);
    assert(cJSON_GetObjectItem(block, "cache_control"));
    assert(strcmp(cJSON_GetObjectItem(block, "text")->valuestring, "extra context") == 0);

    cJSON_Delete(root);
    free(json);
    db_close(db);
    printf(" OK\n");
}

/* Test: session cache_break_after column */
static void test_session_cache_break_col(void) {
    printf("  test_session_cache_break_col...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* Default is -1 */
    assert(session_get_cache_break(db, sid) == -1);

    /* Set and get */
    assert(session_set_cache_break(db, sid, 42) == 0);
    assert(session_get_cache_break(db, sid) == 42);

    /* Update */
    assert(session_set_cache_break(db, sid, 100) == 0);
    assert(session_get_cache_break(db, sid) == 100);

    db_close(db);
    printf(" OK\n");
}

/* Test: OpenAI with tools JSON */
static void test_openai_with_tools(void) {
    printf("  test_openai_with_tools...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message usr = {.role = ROLE_USER, .content = "help"};
    int64_t e1 = entry_append(db, sid, &usr);

    const char *tools = "[{\"type\":\"function\",\"function\":{\"name\":\"file_read\",\"parameters\":{}}}]";
    int64_t ids[] = {e1};
    char *json = db_build_request(db, sid, ids, 1, "m", ENDPOINT_OPENAI,
                                  tools, 0, 0, -1);
    assert(json);

    cJSON *root = cJSON_Parse(json);
    assert(root);
    cJSON *t = cJSON_GetObjectItem(root, "tools");
    assert(t && cJSON_GetArraySize(t) == 1);

    cJSON_Delete(root);
    free(json);
    db_close(db);
    printf(" OK\n");
}

/* Test: special characters in content */
static void test_special_chars(void) {
    printf("  test_special_chars...");
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message usr = {.role = ROLE_USER, .content = "line1\nline2\ttab \"quoted\""};
    int64_t e1 = entry_append(db, sid, &usr);

    int64_t ids[] = {e1};
    char *json = db_build_request(db, sid, ids, 1, "m", ENDPOINT_OPENAI,
                                  NULL, 0, 0, -1);
    assert(json);

    /* Must be valid JSON */
    cJSON *root = cJSON_Parse(json);
    assert(root);
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    const char *content = cJSON_GetObjectItem(m0, "content")->valuestring;
    assert(strstr(content, "line1\nline2") != NULL);
    assert(strstr(content, "\"quoted\"") != NULL);

    cJSON_Delete(root);
    free(json);
    db_close(db);
    printf(" OK\n");
}

int main(void) {
    printf("test_db_request:\n");
    test_openai_basic();
    test_openai_tool_calls();
    test_gemini_basic();
    test_gemini_tool_calls();
    test_openai_cache_break();
    test_session_cache_break_col();
    test_openai_with_tools();
    test_special_chars();
    printf("All tests passed.\n");
    return 0;
}
