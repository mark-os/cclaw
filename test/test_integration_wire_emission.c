/* T177: integration test — wire emission.
 * Insert entries via split columns, run RequestStreamer against mock server,
 * capture request body, verify valid OpenAI JSON (tool_calls format,
 * escaped content w/ newlines/quotes/unicode). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <curl/curl.h>
#include "db.h"
#include "config.h"
#include "context.h"
#include "request_stream.h"
#include "mock_server.h"
#include "http.h"
#include "cJSON.h"
static int s_port;

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* Send streamed request to mock server, return captured body from mock */
static const char *stream_to_mock(sqlite3 *db, int64_t sid, Config *cfg,
                                  ToolSchema *tools, size_t tool_count) {
    ContextPlan plan;
    if (context_plan(db, sid, cfg, &plan) != 0) return NULL;

    RequestStreamer rs;
    if (rs_init(&rs, db, sid, cfg, &plan, tools, tool_count) != 0) {
        context_plan_free(&plan);
        return NULL;
    }

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", s_port);
    const char *headers[] = {"Content-Type: application/json", NULL};

    /* Enqueue a dummy 200 response so mock doesn't return 500 */
    mock_server_enqueue(200, "{\"ok\":true}");

    HttpResponse resp = {0};
    http_post_stream(url, headers, rs_read_cb, &rs, &resp);
    http_response_free(&resp);

    rs_cleanup(&rs);
    context_plan_free(&plan);

    return mock_server_last_request_body();
}

/* T177: content with newlines, quotes, backslashes */
static void test_wire_escaped_content(void) {
    TEST(wire_escaped_content);

    mock_server_reset();

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); }

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* Content with special chars: newlines, tabs, quotes, backslash */
    Message msg = {.role = ROLE_USER,
                   .content = "line1\nline2\t\"quoted\"\r\\back"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.context_window = 100000;

    const char *body = stream_to_mock(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body captured"); }

    /* Parse as JSON — must be valid */
    cJSON *root = cJSON_Parse(body);
    if (!root) { db_close(db); FAIL("invalid JSON"); }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (!messages || cJSON_GetArraySize(messages) < 1) {
        cJSON_Delete(root); db_close(db);
        FAIL("no messages");
    }

    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    const char *content = cJSON_GetObjectItem(m0, "content")->valuestring;
    /* cJSON unescapes — verify round-trip preserves original */
    if (strcmp(content, "line1\nline2\t\"quoted\"\r\\back") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("content mismatch after round-trip");
    }

    cJSON_Delete(root);
    db_close(db);
    PASS();
}

/* T177: content with unicode characters */
static void test_wire_unicode_content(void) {
    TEST(wire_unicode_content);

    mock_server_reset();

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); }

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* UTF-8 content: emoji, CJK, accented chars */
    Message msg = {.role = ROLE_USER,
                   .content = "Hello \xc3\xa9\xc3\xa0\xc3\xbc \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x98\x80"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    const char *body = stream_to_mock(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { db_close(db); FAIL("invalid JSON"); }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    const char *content = cJSON_GetObjectItem(m0, "content")->valuestring;
    /* UTF-8 should pass through unchanged */
    if (strcmp(content, "Hello \xc3\xa9\xc3\xa0\xc3\xbc \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x98\x80") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("unicode content mismatch");
    }

    cJSON_Delete(root);
    db_close(db);
    PASS();
}

/* T177: tool_calls format — OpenAI wire format with stringified arguments */
static void test_wire_tool_calls_format(void) {
    TEST(wire_tool_calls_format);

    mock_server_reset();

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); }

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* User message */
    Message user_msg = {.role = ROLE_USER, .content = "do it"};
    entry_append(db, sid, &user_msg);

    /* Assistant with tool_calls (complex args with nested JSON) */
    ToolCall tcs[2] = {
        {.id = "call_aaa", .name = "file_write",
         .arguments = "{\"path\":\"/tmp/x.txt\",\"content\":\"line1\\nline2\"}"},
        {.id = "call_bbb", .name = "shell_exec",
         .arguments = "{\"cmd\":\"echo \\\"hello\\\"\"}"}
    };
    Message asst = {.role = ROLE_ASSISTANT, .content = "I'll help",
                    .tool_calls = tcs, .tool_call_count = 2,
                    .stop_reason = STOP_REASON_TOOL_USE};
    entry_append(db, sid, &asst);

    /* Tool results */
    ToolResult tr1 = {.tool_call_id = "call_aaa", .content = "ok"};
    Message tool1 = {.role = ROLE_TOOL, .tool_result = &tr1};
    entry_append(db, sid, &tool1);

    ToolResult tr2 = {.tool_call_id = "call_bbb", .content = "hello"};
    Message tool2 = {.role = ROLE_TOOL, .tool_result = &tr2};
    entry_append(db, sid, &tool2);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    const char *body = stream_to_mock(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { db_close(db); FAIL("invalid JSON from wire"); }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (cJSON_GetArraySize(messages) != 4) {
        cJSON_Delete(root); db_close(db);
        FAIL("expected 4 messages");
    }

    /* Verify assistant message tool_calls structure */
    cJSON *m1 = cJSON_GetArrayItem(messages, 1);
    if (strcmp(cJSON_GetObjectItem(m1, "role")->valuestring, "assistant") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("m1 not assistant");
    }

    cJSON *tc_arr = cJSON_GetObjectItem(m1, "tool_calls");
    if (!tc_arr || cJSON_GetArraySize(tc_arr) != 2) {
        cJSON_Delete(root); db_close(db);
        FAIL("expected 2 tool_calls");
    }

    /* First tool_call: verify OpenAI format */
    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    if (strcmp(cJSON_GetObjectItem(tc0, "id")->valuestring, "call_aaa") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("tc0 id mismatch");
    }
    if (strcmp(cJSON_GetObjectItem(tc0, "type")->valuestring, "function") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("tc0 type not function");
    }
    cJSON *fn0 = cJSON_GetObjectItem(tc0, "function");
    if (strcmp(cJSON_GetObjectItem(fn0, "name")->valuestring, "file_write") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("tc0 function name mismatch");
    }
    /* arguments must be a string (OpenAI format: stringified JSON) */
    cJSON *args0 = cJSON_GetObjectItem(fn0, "arguments");
    if (!cJSON_IsString(args0)) {
        cJSON_Delete(root); db_close(db);
        FAIL("tc0 arguments not string");
    }
    /* Parse the stringified args to verify valid JSON */
    cJSON *parsed_args0 = cJSON_Parse(args0->valuestring);
    if (!parsed_args0) {
        cJSON_Delete(root); db_close(db);
        FAIL("tc0 arguments not valid JSON");
    }
    if (strcmp(cJSON_GetObjectItem(parsed_args0, "path")->valuestring, "/tmp/x.txt") != 0) {
        cJSON_Delete(parsed_args0); cJSON_Delete(root); db_close(db);
        FAIL("tc0 args path mismatch");
    }
    cJSON_Delete(parsed_args0);

    /* Second tool_call */
    cJSON *tc1 = cJSON_GetArrayItem(tc_arr, 1);
    if (strcmp(cJSON_GetObjectItem(tc1, "id")->valuestring, "call_bbb") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("tc1 id mismatch");
    }

    /* Verify tool result messages */
    cJSON *m2 = cJSON_GetArrayItem(messages, 2);
    if (strcmp(cJSON_GetObjectItem(m2, "role")->valuestring, "tool") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("m2 not tool");
    }
    if (strcmp(cJSON_GetObjectItem(m2, "tool_call_id")->valuestring, "call_aaa") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("m2 tool_call_id mismatch");
    }

    cJSON *m3 = cJSON_GetArrayItem(messages, 3);
    if (strcmp(cJSON_GetObjectItem(m3, "role")->valuestring, "tool") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("m3 not tool");
    }
    if (strcmp(cJSON_GetObjectItem(m3, "tool_call_id")->valuestring, "call_bbb") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("m3 tool_call_id mismatch");
    }

    cJSON_Delete(root);
    db_close(db);
    PASS();
}

/* T177: multi-turn conversation with mixed content */
static void test_wire_multi_turn(void) {
    TEST(wire_multi_turn);

    mock_server_reset();

    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); }

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* User with special chars */
    Message u1 = {.role = ROLE_USER, .content = "What's 2+2?\n\"Please\" answer."};
    entry_append(db, sid, &u1);

    /* Assistant plain text */
    Message a1 = {.role = ROLE_ASSISTANT, .content = "The answer is 4.\nAnything else?",
                  .stop_reason = STOP_REASON_STOP};
    entry_append(db, sid, &a1);

    /* User follow-up */
    Message u2 = {.role = ROLE_USER, .content = "No, thanks! \xf0\x9f\x91\x8d"};
    entry_append(db, sid, &u2);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    const char *body = stream_to_mock(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { db_close(db); FAIL("invalid JSON"); }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (cJSON_GetArraySize(messages) != 3) {
        cJSON_Delete(root); db_close(db);
        FAIL("expected 3 messages");
    }

    /* Verify roles in order */
    const char *expected_roles[] = {"user", "assistant", "user"};
    for (int i = 0; i < 3; i++) {
        cJSON *m = cJSON_GetArrayItem(messages, i);
        if (strcmp(cJSON_GetObjectItem(m, "role")->valuestring, expected_roles[i]) != 0) {
            cJSON_Delete(root); db_close(db);
            FAIL("role order mismatch");
        }
    }

    /* Verify content round-trips */
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    if (strcmp(cJSON_GetObjectItem(m0, "content")->valuestring,
              "What's 2+2?\n\"Please\" answer.") != 0) {
        cJSON_Delete(root); db_close(db);
        FAIL("m0 content mismatch");
    }

    cJSON_Delete(root);
    db_close(db);
    PASS();
}

int main(void) {
    s_port = mock_server_start();
    printf("test_integration_wire_emission:\n");
    test_wire_escaped_content();
    test_wire_unicode_content();
    test_wire_tool_calls_format();
    test_wire_multi_turn();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    mock_server_stop();
    return tests_passed == tests_run ? 0 : 1;
}
