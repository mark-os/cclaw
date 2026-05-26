#define _POSIX_C_SOURCE 200809L
#include "request_stream.h"
#include "db.h"
#include "config.h"
#include "context.h"
#include "llm.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *test_db_open(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);
    return db;
}

/* Helper: read entire stream into malloc'd buffer */
static char *stream_all(RequestStreamer *rs) {
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    assert(buf);
    while (1) {
        if (len + 1024 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
            assert(buf);
        }
        size_t n = rs_read_cb(buf + len, 1, 1024, rs);
        if (n == 0) break;
        len += n;
    }
    buf[len] = '\0';
    return buf;
}

/* Test: basic preamble + single user message + close */
static void test_basic_user_message(void) {
    printf("  test_basic_user_message...");

    sqlite3 *db;
    db = test_db_open();

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    /* Append a user message */
    Message msg = {.role = ROLE_USER, .content = "hello world"};
    int64_t eid = entry_append(db, sid, &msg);
    assert(eid > 0);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.context_window = 100000;

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);
    assert(plan.count == 1);
    assert(plan.cut == 0); /* all entries included */

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    char *json = stream_all(&rs);
    assert(json);

    /* Parse and validate */
    cJSON *root = cJSON_Parse(json);
    assert(root);
    cJSON *model = cJSON_GetObjectItem(root, "model");
    assert(model && strcmp(model->valuestring, "test-model") == 0);
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    assert(messages && cJSON_GetArraySize(messages) == 1);
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    assert(strcmp(cJSON_GetObjectItem(m0, "role")->valuestring, "user") == 0);
    assert(strcmp(cJSON_GetObjectItem(m0, "content")->valuestring, "hello world") == 0);
    /* V9: no tools field */
    assert(!cJSON_GetObjectItem(root, "tools"));

    cJSON_Delete(root);
    free(json);
    rs_cleanup(&rs);
    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* Test: assistant with tool_calls reshapes correctly */
static void test_assistant_tool_calls(void) {
    printf("  test_assistant_tool_calls...");

    sqlite3 *db;
    db = test_db_open();

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    /* User message */
    Message user_msg = {.role = ROLE_USER, .content = "list files"};
    entry_append(db, sid, &user_msg);

    /* Assistant with tool_call */
    ToolCall tc = {.id = "call_123", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}"};
    Message asst = {.role = ROLE_ASSISTANT, .content = NULL,
                    .tool_calls = &tc, .tool_call_count = 1,
                    .stop_reason = STOP_REASON_TOOL_USE};
    entry_append(db, sid, &asst);

    /* Tool result */
    ToolResult tr = {.tool_call_id = "call_123", .content = "file1.txt\nfile2.txt"};
    Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr};
    entry_append(db, sid, &tool_msg);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.context_window = 100000;

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);
    assert(plan.count == 3);

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    char *json = stream_all(&rs);
    cJSON *root = cJSON_Parse(json);
    assert(root);

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    assert(cJSON_GetArraySize(messages) == 3);

    /* Check assistant message has tool_calls in OpenAI format */
    cJSON *m1 = cJSON_GetArrayItem(messages, 1);
    assert(strcmp(cJSON_GetObjectItem(m1, "role")->valuestring, "assistant") == 0);
    cJSON *tcs = cJSON_GetObjectItem(m1, "tool_calls");
    assert(tcs && cJSON_GetArraySize(tcs) == 1);
    cJSON *tc0 = cJSON_GetArrayItem(tcs, 0);
    assert(strcmp(cJSON_GetObjectItem(tc0, "id")->valuestring, "call_123") == 0);
    cJSON *fn = cJSON_GetObjectItem(tc0, "function");
    assert(fn);
    assert(strcmp(cJSON_GetObjectItem(fn, "name")->valuestring, "shell_exec") == 0);

    /* Check tool result */
    cJSON *m2 = cJSON_GetArrayItem(messages, 2);
    assert(strcmp(cJSON_GetObjectItem(m2, "role")->valuestring, "tool") == 0);
    assert(strcmp(cJSON_GetObjectItem(m2, "tool_call_id")->valuestring, "call_123") == 0);

    cJSON_Delete(root);
    free(json);
    rs_cleanup(&rs);
    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* Test: tools array included when tools provided (V9) */
static void test_tools_included(void) {
    printf("  test_tools_included...");

    sqlite3 *db;
    db = test_db_open();

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    Message msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    ToolSchema tools[] = {{.name = "shell_exec", .description = "run cmd",
                           .parameters_json = "{\"type\":\"object\",\"properties\":{}}"}};

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, tools, 1) == 0);

    char *json = stream_all(&rs);
    cJSON *root = cJSON_Parse(json);
    assert(root);

    cJSON *tools_arr = cJSON_GetObjectItem(root, "tools");
    assert(tools_arr && cJSON_GetArraySize(tools_arr) == 1);
    cJSON *t0 = cJSON_GetArrayItem(tools_arr, 0);
    assert(strcmp(cJSON_GetObjectItem(t0, "type")->valuestring, "function") == 0);
    cJSON *fn = cJSON_GetObjectItem(t0, "function");
    assert(strcmp(cJSON_GetObjectItem(fn, "name")->valuestring, "shell_exec") == 0);

    cJSON_Delete(root);
    free(json);
    rs_cleanup(&rs);
    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* Test: reset allows re-reading from start */
static void test_reset(void) {
    printf("  test_reset...");

    sqlite3 *db;
    db = test_db_open();

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    Message msg = {.role = ROLE_USER, .content = "test reset"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    char *json1 = stream_all(&rs);
    rs_reset(&rs);
    char *json2 = stream_all(&rs);

    assert(strcmp(json1, json2) == 0);

    free(json1);
    free(json2);
    rs_cleanup(&rs);
    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* Test: cutoff notice when plan has truncation */
static void test_cutoff_notice(void) {
    printf("  test_cutoff_notice...");

    sqlite3 *db;
    db = test_db_open();

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    /* Add many messages to force truncation */
    for (int i = 0; i < 20; i++) {
        char buf[256];
        memset(buf, 'A', 255);
        buf[255] = '\0';
        Message msg = {.role = ROLE_USER, .content = buf};
        entry_append(db, sid, &msg);
    }

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 200; /* tiny window to force cut */

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);

    if (plan.cut > 0) {
        /* Truncation occurred — verify cutoff notice */
        RequestStreamer rs;
        assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

        char *json = stream_all(&rs);
        cJSON *root = cJSON_Parse(json);
        assert(root);

        cJSON *messages = cJSON_GetObjectItem(root, "messages");
        /* First message should be the cutoff system notice */
        cJSON *m0 = cJSON_GetArrayItem(messages, 0);
        assert(strcmp(cJSON_GetObjectItem(m0, "role")->valuestring, "system") == 0);
        assert(strstr(cJSON_GetObjectItem(m0, "content")->valuestring, "truncated"));

        cJSON_Delete(root);
        free(json);
        rs_cleanup(&rs);
    }

    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* T166: Test json_escape_into via content with special chars */
static void test_special_chars_in_content(void) {
    printf("  test_special_chars_in_content...");

    sqlite3 *db = test_db_open();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    /* Content with quotes, newlines, tabs, backslashes */
    Message msg = {.role = ROLE_USER, .content = "line1\nline2\t\"quoted\"\r\\end"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    char *json = stream_all(&rs);
    /* Must be valid JSON */
    cJSON *root = cJSON_Parse(json);
    assert(root);

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    const char *c = cJSON_GetObjectItem(m0, "content")->valuestring;
    /* cJSON unescapes — verify round-trip */
    assert(strcmp(c, "line1\nline2\t\"quoted\"\r\\end") == 0);

    cJSON_Delete(root);
    free(json);
    rs_cleanup(&rs);
    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* Test: small read sizes (simulates curl calling with small buffers) */
static void test_small_reads(void) {
    printf("  test_small_reads...");

    sqlite3 *db;
    db = test_db_open();

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    Message msg = {.role = ROLE_USER, .content = "small read test"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    /* Read 1 byte at a time */
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    while (1) {
        if (len + 2 > cap) { cap *= 2; buf = realloc(buf, cap); }
        size_t n = rs_read_cb(buf + len, 1, 1, &rs);
        if (n == 0) break;
        len += n;
    }
    buf[len] = '\0';

    /* Should still be valid JSON */
    cJSON *root = cJSON_Parse(buf);
    assert(root);
    cJSON_Delete(root);

    free(buf);
    rs_cleanup(&rs);
    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* T123: Test cache_control emitted for Anthropic models */
static void test_cache_hints_anthropic(void) {
    printf("  test_cache_hints_anthropic...");

    sqlite3 *db = test_db_open();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    Message msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &msg);

    /* Model name contains "claude" → auto-detect cache hints */
    Config cfg = {0};
    cfg.provider.model = "anthropic/claude-sonnet-4";
    cfg.provider.context_window = 100000;
    cfg.provider.cache_hints = CACHE_HINTS_AUTO;

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    char *json = stream_all(&rs);
    cJSON *root = cJSON_Parse(json);
    assert(root);

    cJSON *cc = cJSON_GetObjectItem(root, "cache_control");
    assert(cc);
    assert(strcmp(cJSON_GetObjectItem(cc, "type")->valuestring, "ephemeral") == 0);

    cJSON_Delete(root);
    free(json);
    rs_cleanup(&rs);
    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* T123: Test cache_control NOT emitted for DeepSeek (auto) */
static void test_cache_hints_deepseek_auto(void) {
    printf("  test_cache_hints_deepseek_auto...");

    sqlite3 *db = test_db_open();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    Message msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.context_window = 100000;
    cfg.provider.cache_hints = CACHE_HINTS_AUTO;

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    char *json = stream_all(&rs);
    cJSON *root = cJSON_Parse(json);
    assert(root);

    /* No cache_control for DeepSeek */
    assert(!cJSON_GetObjectItem(root, "cache_control"));

    cJSON_Delete(root);
    free(json);
    rs_cleanup(&rs);
    context_plan_free(&plan);
    sqlite3_close(db);
    printf(" OK\n");
}

/* T123: Test cache_hints forced on/off via config */
static void test_cache_hints_forced(void) {
    printf("  test_cache_hints_forced...");

    sqlite3 *db = test_db_open();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    Message msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &msg);

    /* Force ON for non-Anthropic model */
    Config cfg = {0};
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.context_window = 100000;
    cfg.provider.cache_hints = CACHE_HINTS_ON;

    ContextPlan plan;
    assert(context_plan(db, sid, &cfg, &plan) == 0);

    RequestStreamer rs;
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    char *json = stream_all(&rs);
    cJSON *root = cJSON_Parse(json);
    assert(root);
    assert(cJSON_GetObjectItem(root, "cache_control"));
    cJSON_Delete(root);
    free(json);
    rs_cleanup(&rs);
    context_plan_free(&plan);

    /* Force OFF for Anthropic model */
    cfg.provider.model = "anthropic/claude-sonnet-4";
    cfg.provider.cache_hints = CACHE_HINTS_OFF;

    assert(context_plan(db, sid, &cfg, &plan) == 0);
    assert(rs_init(&rs, db, sid, &cfg, &plan, NULL, 0) == 0);

    json = stream_all(&rs);
    root = cJSON_Parse(json);
    assert(root);
    assert(!cJSON_GetObjectItem(root, "cache_control"));
    cJSON_Delete(root);
    free(json);
    rs_cleanup(&rs);
    context_plan_free(&plan);

    sqlite3_close(db);
    printf(" OK\n");
}

int main(void) {
    printf("test_request_stream:\n");
    test_basic_user_message();
    test_assistant_tool_calls();
    test_tools_included();
    test_reset();
    test_cutoff_notice();
    test_special_chars_in_content();
    test_small_reads();
    test_cache_hints_anthropic();
    test_cache_hints_deepseek_auto();
    test_cache_hints_forced();
    printf("All request_stream tests passed.\n");
    return 0;
}
