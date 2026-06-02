/* T256/T257: Test cclaw extension API — registerTool, registerHook */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "extension.h"
#include "hook_dispatch.h"
#include "tool_js.h"
#include "tools.h"
#include <mquickjs.h>

static int tests_run = 0;
static int tests_passed = 0;

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

static void cleanup(const char *ws) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    (void)system(cmd);
}

/* V111: registerTool registers a tool callable from the agent */
static void test_register_tool(void) {
    TEST(register_tool);
    const char *ws = "/tmp/cclaw_ext_api_t1";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/mytool.js", ext_dir);
    write_file(p1,
        "cclaw.registerTool({\n"
        "  name: 'greet',\n"
        "  description: 'Say hello',\n"
        "  parameters: {type:'object',properties:{name:{type:'string'}}},\n"
        "  handler: function(args) { return 'hello ' + (args.name || 'world'); }\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt");

    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    if (count != 1) { js_runtime_destroy(rt); cleanup(ws); FAIL("discover"); }

    Config cfg = {.log_level = LOG_LEVEL_INFO};
    int loaded = extension_load(paths, count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, count);

    if (loaded != 1) { js_runtime_destroy(rt); cleanup(ws); FAIL("load"); }

    /* Tool should be registered */
    ToolEntry *e = tools_lookup(&reg, "greet");
    if (!e) { js_runtime_destroy(rt); cleanup(ws); FAIL("tool not registered"); }

    /* Invoke it */
    char *result = e->handler("{\"name\":\"alice\"}", e->user_data);
    if (!result || strcmp(result, "hello alice") != 0) {
        printf("got: %s ", result ? result : "NULL");
        free(result);
        tools_free(&reg);
        extension_ctx_destroy(&ext_ctx);
        js_runtime_destroy(rt);
        cleanup(ws);
        FAIL("wrong result");
    }
    free(result);

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* V112: registerHook stores hooks per event */
static void test_register_hook(void) {
    TEST(register_hook);
    const char *ws = "/tmp/cclaw_ext_api_t2";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/hooks.js", ext_dir);
    write_file(p1,
        "cclaw.registerHook('beforeToolCall', function(ctx) { return ctx; });\n"
        "cclaw.registerHook('afterToolCall', function(ctx) { return ctx; });\n"
        "cclaw.registerHook('beforeToolCall', function(ctx) { return ctx; });\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt");

    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, count);

    /* beforeToolCall should have 2, afterToolCall should have 1 */
    if (ext_ctx.hooks[HOOK_BEFORE_TOOL_CALL].count != 2) {
        printf("got %zu ", ext_ctx.hooks[HOOK_BEFORE_TOOL_CALL].count);
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected 2 beforeToolCall hooks");
    }
    if (ext_ctx.hooks[HOOK_AFTER_TOOL_CALL].count != 1) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected 1 afterToolCall hook");
    }
    /* Other events should be 0 */
    if (ext_ctx.hooks[HOOK_TURN_START].count != 0) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected 0 turnStart hooks");
    }

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* V109: Extension that throws during registerTool → skipped, others load */
static void test_register_tool_invalid(void) {
    TEST(register_tool_invalid);
    const char *ws = "/tmp/cclaw_ext_api_t3";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/bad.js", ext_dir);
    /* Missing handler → throws */
    write_file(p1, "cclaw.registerTool({name: 'broken'});");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt");
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    int loaded = extension_load(paths, count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, count);

    /* Extension threw → loaded == 0 */
    if (loaded != 0) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected 0 loaded");
    }
    /* No tool registered */
    if (tools_lookup(&reg, "broken") != NULL) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("broken tool should not be registered");
    }

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* hook_event_from_name utility */
static void test_hook_event_from_name(void) {
    TEST(hook_event_from_name);
    if (hook_event_from_name("beforeRequest") != HOOK_BEFORE_REQUEST) FAIL("beforeRequest");
    if (hook_event_from_name("afterResponse") != HOOK_AFTER_RESPONSE) FAIL("afterResponse");
    if (hook_event_from_name("beforeToolCall") != HOOK_BEFORE_TOOL_CALL) FAIL("beforeToolCall");
    if (hook_event_from_name("afterToolCall") != HOOK_AFTER_TOOL_CALL) FAIL("afterToolCall");
    if (hook_event_from_name("turnStart") != HOOK_TURN_START) FAIL("turnStart");
    if (hook_event_from_name("turnEnd") != HOOK_TURN_END) FAIL("turnEnd");
    if (hook_event_from_name("invalid") != -1) FAIL("invalid should be -1");
    if (hook_event_from_name(NULL) != -1) FAIL("NULL should be -1");
    PASS();
}

/* T260: turnStart/turnEnd hooks fire without error */
static void test_turn_start_end_hooks(void) {
    TEST(turn_start_end_hooks);
    const char *ws = "/tmp/cclaw_ext_api_t5";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/lifecycle.js", ext_dir);
    write_file(p1,
        "globalThis.__turn_started = false;\n"
        "globalThis.__turn_ended = false;\n"
        "cclaw.registerHook('turnStart', function() { globalThis.__turn_started = true; });\n"
        "cclaw.registerHook('turnEnd', function() { globalThis.__turn_ended = true; });\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt");
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, count);

    if (ext_ctx.hooks[HOOK_TURN_START].count != 1) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected 1 turnStart hook");
    }

    /* Dispatch turnStart */
    hook_dispatch_turn_start(&ext_ctx);

    /* Verify global was set */
    JSContext *ctx = (JSContext *)rt->ctx;
    const char *chk = "globalThis.__turn_started ? 'yes' : 'no'";
    JSValue v = JS_Eval(ctx, chk, strlen(chk), "<test>", JS_EVAL_RETVAL);
    JSCStringBuf buf;
    const char *s = JS_ToCString(ctx, v, &buf);
    if (!s || strcmp(s, "yes") != 0) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("turnStart hook did not fire");
    }

    /* Dispatch turnEnd */
    hook_dispatch_turn_end(&ext_ctx);
    chk = "globalThis.__turn_ended ? 'yes' : 'no'";
    v = JS_Eval(ctx, chk, strlen(chk), "<test>", JS_EVAL_RETVAL);
    s = JS_ToCString(ctx, v, &buf);
    if (!s || strcmp(s, "yes") != 0) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("turnEnd hook did not fire");
    }

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* T261: afterResponse hook receives response object */
static void test_after_response_hook(void) {
    TEST(after_response_hook);
    const char *ws = "/tmp/cclaw_ext_api_t6";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/resp.js", ext_dir);
    write_file(p1,
        "globalThis.__last_resp = null;\n"
        "cclaw.registerHook('afterResponse', function(resp) {\n"
        "  globalThis.__last_resp = resp;\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt");
    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, count);

    if (ext_ctx.hooks[HOOK_AFTER_RESPONSE].count != 1) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("expected 1 afterResponse hook");
    }

    /* Dispatch */
    hook_dispatch_after_response(&ext_ctx, "Hello world", "stop", 0);

    /* Verify */
    JSContext *ctx = (JSContext *)rt->ctx;
    const char *chk = "globalThis.__last_resp ? globalThis.__last_resp.content : 'NONE'";
    JSValue v = JS_Eval(ctx, chk, strlen(chk), "<test>", JS_EVAL_RETVAL);
    JSCStringBuf buf;
    const char *s = JS_ToCString(ctx, v, &buf);
    if (!s || strcmp(s, "Hello world") != 0) {
        printf("got: %s ", s ? s : "NULL");
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("afterResponse content mismatch");
    }

    chk = "globalThis.__last_resp.finish_reason";
    v = JS_Eval(ctx, chk, strlen(chk), "<test>", JS_EVAL_RETVAL);
    s = JS_ToCString(ctx, v, &buf);
    if (!s || strcmp(s, "stop") != 0) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("afterResponse finish_reason mismatch");
    }

    chk = "globalThis.__last_resp.tool_call_count";
    v = JS_Eval(ctx, chk, strlen(chk), "<test>", JS_EVAL_RETVAL);
    int32_t tc_count;
    if (JS_ToInt32(ctx, &tc_count, v) != 0 || tc_count != 0) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("afterResponse tool_call_count mismatch");
    }

    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

int main(void) {
    printf("test_extension_api:\n");
    test_register_tool();
    test_register_hook();
    test_register_tool_invalid();
    test_hook_event_from_name();
    test_turn_start_end_hooks();
    test_after_response_hook();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
