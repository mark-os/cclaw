/* T237/B2: verify max_tokens present in wire JSON when tools are registered.
 * Unit test — reads from RequestStreamer directly, no mock server. */
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

static char *stream_to_buf(sqlite3 *db, int64_t sid, Config *cfg,
                           ToolSchema *tools, size_t tool_count) {
    ContextPlan plan;
    if (context_plan(db, sid, cfg, 0, &plan) != 0) return NULL;

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

static void test_max_tokens_with_tools(void) {
    TEST(max_tokens_with_tools);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 4096;
    cfg.provider.context_window = 100000;

    ToolSchema tools[] = {{
        .name = "shell_exec",
        .description = "run a command",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"cmd\":{\"type\":\"string\"}}}"
    }};

    char *body = stream_to_buf(db, sid, &cfg, tools, 1);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { free(body); db_close(db); FAIL("invalid JSON"); }

    cJSON *mt = cJSON_GetObjectItem(root, "max_tokens");
    if (!mt || !cJSON_IsNumber(mt) || mt->valueint != 4096) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("max_tokens missing or wrong when tools present");
    }

    cJSON *t = cJSON_GetObjectItem(root, "tools");
    if (!t || !cJSON_IsArray(t) || cJSON_GetArraySize(t) != 1) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("tools array missing or wrong size");
    }

    cJSON_Delete(root); free(body); db_close(db);
    PASS();
}

static void test_max_tokens_without_tools(void) {
    TEST(max_tokens_without_tools);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.max_tokens = 2048;
    cfg.provider.context_window = 100000;

    char *body = stream_to_buf(db, sid, &cfg, NULL, 0);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { free(body); db_close(db); FAIL("invalid JSON"); }

    cJSON *mt = cJSON_GetObjectItem(root, "max_tokens");
    if (!mt || mt->valueint != 2048) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("max_tokens missing or wrong without tools");
    }

    /* V9: no tools field */
    if (cJSON_GetObjectItem(root, "tools")) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("tools field present when no tools registered");
    }

    cJSON_Delete(root); free(body); db_close(db);
    PASS();
}

static void test_no_max_tokens_when_zero(void) {
    TEST(no_max_tokens_when_zero);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &msg);

    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.max_tokens = 0;
    cfg.provider.context_window = 100000;

    ToolSchema tools[] = {{.name = "test", .description = "t", .parameters_json = "{}"}};
    char *body = stream_to_buf(db, sid, &cfg, tools, 1);
    if (!body) { db_close(db); FAIL("no body"); }

    cJSON *root = cJSON_Parse(body);
    if (!root) { free(body); db_close(db); FAIL("invalid JSON"); }

    if (cJSON_GetObjectItem(root, "max_tokens")) {
        cJSON_Delete(root); free(body); db_close(db);
        FAIL("max_tokens should be absent when 0");
    }

    cJSON_Delete(root); free(body); db_close(db);
    PASS();
}

int main(void) {
    alarm(10);
    printf("test_wire_tools_max_tokens (T237/B2):\n");
    test_max_tokens_with_tools();
    test_max_tokens_without_tools();
    test_no_max_tokens_when_zero();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
