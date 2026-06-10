/* T258: Test beforeRequest hook dispatch.
 * V112: hooks receive messages array, can modify/add/remove, chain in load order. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "hook_dispatch.h"
#include "extension.h"
#include "tool_js.h"
#include "tools.h"
#include "db.h"

static int tests_run = 0;
static int tests_passed = 0;
static sqlite3 *g_hook_db;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

static void cleanup(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    (void)system(cmd);
}

/* No hooks registered → returns NULL */
static void test_no_hooks(void) {
    TEST(no_hooks_returns_null);

    ExtensionCtx ext_ctx;
    JsSessionRuntime *rt = js_runtime_create();
    extension_ctx_init(&ext_ctx, rt);
    /* No hooks registered */

    Config cfg = {.log_level = LOG_LEVEL_INFO,
                  .provider = {.model = "test-model", .max_tokens = 100}};
    ContextPlan plan = {.entries = NULL, .count = 0, .cut = 0, .budget = 1000};

    char *result = hook_dispatch_before_request(&ext_ctx, NULL, 1, &cfg, &plan, NULL, 0);
    if (result != NULL) {
        free(result);
        extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt);
        FAIL("expected NULL when no hooks");
    }

    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    PASS();
}

/* Hook modifies messages — adds a system message */
static void test_hook_modifies_messages(void) {
    TEST(hook_modifies_messages);

    const char *ws = "/tmp/cclaw_hook_t1";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Extension: prepend a system message */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/inject.js", ext_dir);
    write_file(p1,
        "cclaw.registerHook('beforeRequest', function(msgs) {\n"
        "  msgs.unshift({role: 'system', content: 'injected by hook'});\n"
        "  return msgs;\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt create");

    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {.log_level = LOG_LEVEL_INFO,
                  .provider = {.model = "test-model", .max_tokens = 100}};
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);

    /* Verify hook registered */
    if (ext_ctx.hooks[HOOK_BEFORE_REQUEST].count != 1) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected 1 hook");
    }

    /* Create a test DB with entries */
    const char *db_path = "/tmp/cclaw_hook_t1.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);
    if (!db) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("db open");
    }

    /* Create session */
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    if (sid < 0) {
        sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("session create");
    }

    /* Append a user message */
    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    /* Build plan */
    ContextPlan plan;
    int rc = context_plan(db, sid, &cfg, 0, &plan);
    if (rc != 0) {
        sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("context_plan");
    }

    /* Dispatch hook */
    char *body = hook_dispatch_before_request(&ext_ctx, db, sid, &cfg, &plan, NULL, 0);
    context_plan_free(&plan);

    if (!body) {
        sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("expected non-NULL body");
    }

    /* Verify the body contains the injected message */
    if (!strstr(body, "injected by hook")) {
        printf("body: %.200s ", body);
        free(body); sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("missing injected message");
    }

    /* Verify it also contains the original user message */
    if (!strstr(body, "hello")) {
        free(body); sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("missing original message");
    }

    /* Verify it has model */
    if (!strstr(body, "test-model")) {
        free(body); sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("missing model");
    }

    free(body);
    sqlite3_close(db);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    unlink(db_path);
    PASS();
}

/* Hook throws → skipped, messages pass through unmodified */
static void test_hook_throws(void) {
    TEST(hook_throws_skipped);

    const char *ws = "/tmp/cclaw_hook_t2";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Extension: throws */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/bad.js", ext_dir);
    write_file(p1,
        "cclaw.registerHook('beforeRequest', function(msgs) {\n"
        "  throw new Error('oops');\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt");
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {.log_level = LOG_LEVEL_INFO,
                  .provider = {.model = "test-model", .max_tokens = 50}};
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);

    /* Create DB with entry */
    const char *db_path = "/tmp/cclaw_hook_t2.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "hi"};
    entry_append(db, sid, &user_msg);

    ContextPlan plan;
    context_plan(db, sid, &cfg, 0, &plan);

    /* Should still produce valid body (hook threw but messages remain) */
    char *body = hook_dispatch_before_request(&ext_ctx, db, sid, &cfg, &plan, NULL, 0);
    context_plan_free(&plan);

    if (!body) {
        sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("expected body even on throw");
    }

    /* Should still have the user message */
    if (!strstr(body, "hi")) {
        free(body); sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("missing original msg");
    }

    free(body);
    sqlite3_close(db);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    unlink(db_path);
    PASS();
}

/* Multiple hooks chain in load order */
static void test_hooks_chain(void) {
    TEST(hooks_chain_in_order);

    const char *ws = "/tmp/cclaw_hook_t3";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Two hooks: first adds "first", second adds "second" */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/chain.js", ext_dir);
    write_file(p1,
        "cclaw.registerHook('beforeRequest', function(msgs) {\n"
        "  msgs.push({role: 'system', content: 'FIRST'});\n"
        "  return msgs;\n"
        "});\n"
        "cclaw.registerHook('beforeRequest', function(msgs) {\n"
        "  msgs.push({role: 'system', content: 'SECOND'});\n"
        "  return msgs;\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {.log_level = LOG_LEVEL_INFO,
                  .provider = {.model = "test-model"}};
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);

    if (ext_ctx.hooks[HOOK_BEFORE_REQUEST].count != 2) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected 2 hooks");
    }

    const char *db_path = "/tmp/cclaw_hook_t3.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "test"};
    entry_append(db, sid, &user_msg);

    ContextPlan plan;
    context_plan(db, sid, &cfg, 0, &plan);
    char *body = hook_dispatch_before_request(&ext_ctx, db, sid, &cfg, &plan, NULL, 0);
    context_plan_free(&plan);

    if (!body) {
        sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("expected body");
    }

    /* Both should be present */
    if (!strstr(body, "FIRST") || !strstr(body, "SECOND")) {
        printf("body: %.300s ", body);
        free(body); sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("missing hook messages");
    }

    /* FIRST should appear before SECOND (chaining order) */
    char *first_pos = strstr(body, "FIRST");
    char *second_pos = strstr(body, "SECOND");
    if (first_pos >= second_pos) {
        free(body); sqlite3_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); unlink(db_path); FAIL("wrong chain order");
    }

    free(body);
    sqlite3_close(db);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    unlink(db_path);
    PASS();
}

/* V113: beforeToolCall blocks execution */
static void test_before_tool_call_blocks(void) {
    TEST(before_tool_call_blocks);

    const char *ws = "/tmp/cclaw_hook_t4";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/block.js", ext_dir);
    write_file(p1,
        "cclaw.registerHook('beforeToolCall', function(ctx) {\n"
        "  if (ctx.name === 'dangerous') return {block: true, reason: 'not allowed'};\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);

    /* Call for blocked tool */
    char *blocked = hook_dispatch_before_tool_call(&ext_ctx, g_hook_db, "dangerous", "{}");
    if (!blocked) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected block");
    }
    if (!strstr(blocked, "not allowed")) {
        printf("got: %s ", blocked);
        free(blocked); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("missing reason");
    }
    free(blocked);

    /* Call for allowed tool */
    char *allowed = hook_dispatch_before_tool_call(&ext_ctx, g_hook_db, "safe_tool", "{}");
    if (allowed) {
        free(allowed); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("expected NULL for allowed tool");
    }

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* V114: afterToolCall replaces result */
static void test_after_tool_call_replaces(void) {
    TEST(after_tool_call_replaces);

    const char *ws = "/tmp/cclaw_hook_t5";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/replace.js", ext_dir);
    write_file(p1,
        "cclaw.registerHook('afterToolCall', function(ctx) {\n"
        "  if (ctx.name === 'fetch') return {result: 'replaced: ' + ctx.result};\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);

    /* Call for matching tool */
    char *replaced = hook_dispatch_after_tool_call(&ext_ctx, g_hook_db, "fetch", "{}", "original data");
    if (!replaced) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected replacement");
    }
    if (strcmp(replaced, "replaced: original data") != 0) {
        printf("got: %s ", replaced);
        free(replaced); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("wrong replacement");
    }
    free(replaced);

    /* Non-matching tool → no replacement */
    char *noop = hook_dispatch_after_tool_call(&ext_ctx, g_hook_db, "other", "{}", "data");
    if (noop) {
        free(noop); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("expected NULL for non-matching");
    }

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* V114: afterToolCall hooks chain — each sees previous result */
static void test_after_tool_call_chains(void) {
    TEST(after_tool_call_chains);

    const char *ws = "/tmp/cclaw_hook_t6";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/chain.js", ext_dir);
    write_file(p1,
        "cclaw.registerHook('afterToolCall', function(ctx) {\n"
        "  return {result: ctx.result + '+A'};\n"
        "});\n"
        "cclaw.registerHook('afterToolCall', function(ctx) {\n"
        "  return {result: ctx.result + '+B'};\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);

    char *result = hook_dispatch_after_tool_call(&ext_ctx, g_hook_db, "any", "{}", "start");
    if (!result) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected result");
    }
    /* First hook: "start+A", second sees that and returns "start+A+B" */
    if (strcmp(result, "start+A+B") != 0) {
        printf("got: %s ", result);
        free(result); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("wrong chain result");
    }
    free(result);

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

int main(void) {
    printf("test_hook_dispatch:\n");
    sqlite3_open(":memory:", &g_hook_db);
    test_no_hooks();
    test_hook_modifies_messages();
    test_hook_throws();
    test_hooks_chain();
    test_before_tool_call_blocks();
    test_after_tool_call_replaces();
    test_after_tool_call_chains();
    sqlite3_close(g_hook_db);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
