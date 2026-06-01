/* T177: unit test — wire emission.
 * Insert entries via split columns, run RequestStreamer into buffer,
 * verify valid OpenAI JSON (tool_calls format, escaped content). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "db.h"
#include "config.h"
#include "context.h"
#include "request_stream.h"
#include "cJSON.h"

static int tests_run = 0, tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

/* Read entire streamer output into heap buffer */
static char *stream_to_buf(sqlite3 *db, int64_t sid, Config *cfg,
                           ToolSchema *tools, size_t tool_count) {
    ContextPlan plan;
    if (context_plan(db, sid, cfg, &plan) != 0) return NULL;

    RequestStreamer rs;
    if (rs_init(&rs, db, sid, cfg, &plan, tools, tool_count) != 0) {
        context_plan_free(&plan);
        return NULL;
    }

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    while (buf) {
        if (len + 1024 > cap) { cap *= 2; buf = realloc(buf, cap); }
        size_t n = rs_read_cb(buf + len, 1, 1024, &rs);
        if (n == 0) break;
        len += n;
    }
    if (buf) buf[len] = '\0';

    rs_cleanup(&rs);
    context_plan_free(&plan);
    return buf;
}

static void test_wire_escaped_content(void) {
    TEST(wire_escaped_content);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message msg = {.role = ROLE_USER,
                   .content = "line1\nline2\t\"quoted\"\r\\back"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.context_window = 100000;

    char *body = stream_to_buf(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { free(body); db_close(db); FAIL("invalid JSON"); }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    const char *content = cJSON_GetObjectItem(m0, "content")->valuestring;
    if (strcmp(content, "line1\nline2\t\"quoted\"\r\\back") != 0) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("content mismatch after round-trip");
    }

    cJSON_Delete(root); free(body); db_close(db);
    PASS();
}

static void test_wire_unicode_content(void) {
    TEST(wire_unicode_content);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message msg = {.role = ROLE_USER,
                   .content = "Hello \xc3\xa9\xc3\xa0\xc3\xbc \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x98\x80"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    char *body = stream_to_buf(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { free(body); db_close(db); FAIL("invalid JSON"); }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    cJSON *m0 = cJSON_GetArrayItem(messages, 0);
    const char *content = cJSON_GetObjectItem(m0, "content")->valuestring;
    if (strcmp(content, "Hello \xc3\xa9\xc3\xa0\xc3\xbc \xe4\xb8\x96\xe7\x95\x8c \xf0\x9f\x98\x80") != 0) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("unicode content mismatch");
    }

    cJSON_Delete(root); free(body); db_close(db);
    PASS();
}

static void test_wire_tool_calls_format(void) {
    TEST(wire_tool_calls_format);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message user_msg = {.role = ROLE_USER, .content = "do it"};
    entry_append(db, sid, &user_msg);

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

    ToolResult tr1 = {.tool_call_id = "call_aaa", .content = "ok"};
    Message tool1 = {.role = ROLE_TOOL, .tool_result = &tr1};
    entry_append(db, sid, &tool1);

    ToolResult tr2 = {.tool_call_id = "call_bbb", .content = "hello"};
    Message tool2 = {.role = ROLE_TOOL, .tool_result = &tr2};
    entry_append(db, sid, &tool2);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    char *body = stream_to_buf(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { free(body); db_close(db); FAIL("invalid JSON"); }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (cJSON_GetArraySize(messages) != 4) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("expected 4 messages");
    }

    /* Verify assistant tool_calls */
    cJSON *m1 = cJSON_GetArrayItem(messages, 1);
    cJSON *tc_arr = cJSON_GetObjectItem(m1, "tool_calls");
    if (!tc_arr || cJSON_GetArraySize(tc_arr) != 2) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("expected 2 tool_calls");
    }

    cJSON *tc0 = cJSON_GetArrayItem(tc_arr, 0);
    if (strcmp(cJSON_GetObjectItem(tc0, "id")->valuestring, "call_aaa") != 0) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("tc0 id mismatch");
    }
    if (strcmp(cJSON_GetObjectItem(tc0, "type")->valuestring, "function") != 0) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("tc0 type not function");
    }
    cJSON *fn0 = cJSON_GetObjectItem(tc0, "function");
    if (strcmp(cJSON_GetObjectItem(fn0, "name")->valuestring, "file_write") != 0) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("tc0 function name mismatch");
    }
    cJSON *args0 = cJSON_GetObjectItem(fn0, "arguments");
    if (!cJSON_IsString(args0)) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("tc0 arguments not string");
    }
    cJSON *parsed_args0 = cJSON_Parse(args0->valuestring);
    if (!parsed_args0) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("tc0 arguments not valid JSON");
    }
    if (strcmp(cJSON_GetObjectItem(parsed_args0, "path")->valuestring, "/tmp/x.txt") != 0) {
        cJSON_Delete(parsed_args0); cJSON_Delete(root); free(body); db_close(db);
        FAIL("tc0 args path mismatch");
    }
    cJSON_Delete(parsed_args0);

    /* Verify tool result messages */
    cJSON *m2 = cJSON_GetArrayItem(messages, 2);
    if (strcmp(cJSON_GetObjectItem(m2, "role")->valuestring, "tool") != 0 ||
        strcmp(cJSON_GetObjectItem(m2, "tool_call_id")->valuestring, "call_aaa") != 0) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("m2 tool mismatch");
    }

    cJSON_Delete(root); free(body); db_close(db);
    PASS();
}

static void test_wire_multi_turn(void) {
    TEST(wire_multi_turn);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message u1 = {.role = ROLE_USER, .content = "What's 2+2?\n\"Please\" answer."};
    entry_append(db, sid, &u1);
    Message a1 = {.role = ROLE_ASSISTANT, .content = "The answer is 4.\nAnything else?",
                  .stop_reason = STOP_REASON_STOP};
    entry_append(db, sid, &a1);
    Message u2 = {.role = ROLE_USER, .content = "No, thanks! \xf0\x9f\x91\x8d"};
    entry_append(db, sid, &u2);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.context_window = 100000;

    char *body = stream_to_buf(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { free(body); db_close(db); FAIL("invalid JSON"); }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (cJSON_GetArraySize(messages) != 3) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("expected 3 messages");
    }

    const char *expected_roles[] = {"user", "assistant", "user"};
    for (int i = 0; i < 3; i++) {
        cJSON *m = cJSON_GetArrayItem(messages, i);
        if (strcmp(cJSON_GetObjectItem(m, "role")->valuestring, expected_roles[i]) != 0) {
            cJSON_Delete(root); free(body); db_close(db);
            FAIL("role order mismatch");
        }
    }

    cJSON_Delete(root); free(body); db_close(db);
    PASS();
}

int main(void) {
    alarm(10);
    printf("test_wire_emission (T177):\n");
    test_wire_escaped_content();
    test_wire_unicode_content();
    test_wire_tool_calls_format();
    test_wire_multi_turn();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
