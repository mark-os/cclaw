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
#include "test_util.h"
#include <mquickjs.h>

static int tests_run = 0;
static int tests_passed = 0;
static sqlite3 *g_hook_db;

/* Read a JS global expression to a C string (heap, caller frees) or NULL. */
static char *js_read_global(JsSessionRuntime *rt, const char *expr) {
    JSContext *ctx = (JSContext *)rt->ctx;
    JSValue v = JS_Eval(ctx, expr, strlen(expr), "<t>", JS_EVAL_RETVAL);
    if (JS_IsException(v)) return NULL;
    JSCStringBuf buf;
    const char *s = JS_ToCString(ctx, v, &buf);
    return s ? strdup(s) : NULL;
}

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
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx, NULL);
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
    entry_append_with_turn(db, sid, &user_msg, 1);

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
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx, NULL);
    extension_list_free(paths, ext_count);

    /* Create DB with entry */
    const char *db_path = "/tmp/cclaw_hook_t2.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "hi"};
    entry_append_with_turn(db, sid, &user_msg, 1);

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
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx, NULL);
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
    entry_append_with_turn(db, sid, &user_msg, 1);

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

/* §8 gating hook: restrict-only {allow|ask|deny}, most-restrictive wins. */
static void test_gate_restrict_only(void) {
    TEST(gate_restrict_only);

    const char *ws = "/tmp/cclaw_hook_t4";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/gate.js", ext_dir);
    write_file(p1,
        "cclaw.registerHook('beforeToolCall', function(ctx) {\n"
        "  if (ctx.name === 'dangerous') return {deny: true, reason: 'not allowed'};\n"
        "  if (ctx.name === 'risky') return {ask: true, reason: 'confirm please'};\n"
        "  if (ctx.name === 'legacy') return {block: true};\n"  /* block aliases deny */
        "  if (ctx.name === 'greedy') return {allow: true};\n"  /* cannot relax */
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx, NULL);
    extension_list_free(paths, ext_count);

    char *reason = NULL;
    HookGate g;

    g = hook_dispatch_gate_tool_call(&ext_ctx, g_hook_db, "dangerous", "{}", &reason);
    if (g != HOOK_GATE_DENY || !reason || strcmp(reason, "not allowed") != 0) {
        free(reason); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("deny+reason expected");
    }
    free(reason); reason = NULL;

    g = hook_dispatch_gate_tool_call(&ext_ctx, g_hook_db, "risky", "{}", &reason);
    if (g != HOOK_GATE_ASK || !reason || strcmp(reason, "confirm please") != 0) {
        free(reason); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("ask+reason expected");
    }
    free(reason); reason = NULL;

    g = hook_dispatch_gate_tool_call(&ext_ctx, g_hook_db, "legacy", "{}", NULL);
    if (g != HOOK_GATE_DENY) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("block should map to deny");
    }

    /* allow / unknown / safe → ALLOW (a hook can never raise authority) */
    if (hook_dispatch_gate_tool_call(&ext_ctx, g_hook_db, "greedy", "{}", NULL) != HOOK_GATE_ALLOW ||
        hook_dispatch_gate_tool_call(&ext_ctx, g_hook_db, "safe", "{}", NULL) != HOOK_GATE_ALLOW) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("allow/unknown should be ALLOW");
    }

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* §8 observer hook: side-effect only — runs, but cannot modify the result. */
static void test_observer_side_effect_only(void) {
    TEST(observer_side_effect_only);

    const char *ws = "/tmp/cclaw_hook_t5";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/observe.js", ext_dir);
    /* Records what it saw into a global, and *tries* to return a replacement
     * (which the dispatcher must ignore — observers cannot gate/modify). */
    write_file(p1,
        "globalThis.__seen = '';\n"
        "cclaw.registerHook('afterToolCall', function(ctx) {\n"
        "  globalThis.__seen = ctx.name + '=' + ctx.result;\n"
        "  return {result: 'SHOULD BE IGNORED'};\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx, NULL);
    extension_list_free(paths, ext_count);

    /* Returns void — there is no way for an observer to alter the result. */
    hook_dispatch_observe_tool_call(&ext_ctx, g_hook_db, "fetch", "{}", "original data");

    char *seen = js_read_global(rt, "globalThis.__seen");
    if (!seen || strcmp(seen, "fetch=original data") != 0) {
        printf("seen: %s ", seen ? seen : "(null)");
        free(seen); tools_free(&reg); extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt); cleanup(ws); FAIL("observer did not run / saw wrong data");
    }
    free(seen);

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* No tool-call hooks registered → gate ALLOWs, observer is a no-op. */
static void test_no_tool_hooks(void) {
    TEST(no_tool_hooks);
    ExtensionCtx ext_ctx;
    JsSessionRuntime *rt = js_runtime_create();
    extension_ctx_init(&ext_ctx, rt);

    char *reason = (char *)0x1;  /* must be NULLed even when no hooks */
    HookGate g = hook_dispatch_gate_tool_call(&ext_ctx, g_hook_db, "x", "{}", &reason);
    int ok = (g == HOOK_GATE_ALLOW && reason == NULL);
    hook_dispatch_observe_tool_call(&ext_ctx, g_hook_db, "x", "{}", "r");  /* no crash */

    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    if (!ok) FAIL("expected ALLOW + NULL reason with no hooks");
    PASS();
}

int main(void) {
    printf("test_hook_dispatch:\n");
    sqlite3_open(":memory:", &g_hook_db);
    test_no_hooks();
    test_hook_modifies_messages();
    test_hook_throws();
    test_hooks_chain();
    test_gate_restrict_only();
    test_observer_side_effect_only();
    test_no_tool_hooks();
    sqlite3_close(g_hook_db);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
