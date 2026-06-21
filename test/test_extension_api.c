/* T256/T257: Test cclaw extension API — registerTool, registerHook */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sqlite3.h>
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

/* Set CCLAW_MJS_EXE to sibling cclaw binary (same pattern as test_tool_js) */
static void setup_mjs_env(void) {
    char self[4096];
    ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) return;
    self[n] = '\0';
    char *slash = strrchr(self, '/');
    if (slash) slash[1] = '\0'; else self[0] = '\0';
    char cclaw_path[4128];
    snprintf(cclaw_path, sizeof(cclaw_path), "%scclaw", self);
    setenv("CCLAW_MJS_EXE", cclaw_path, 1);
    setenv("CCLAW_MJS_HOST", "1", 1);
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
    snprintf(p1, sizeof(p1), "%s/mytool.mjs", ext_dir);
    write_file(p1,
        "cclaw.registerTool({\n"
        "  name: 'greet',\n"
        "  description: 'Say hello',\n"
        "  parameters: {type:'object',properties:{name:{type:'string'}}},\n"
        "  handler: \"return 'hello ' + (args.name || 'world');\"\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt");

    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    JsEvalCtx ectx = {0};
    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    if (count != 1) { js_runtime_destroy(rt); cleanup(ws); FAIL("discover"); }

    Config cfg = {.log_level = LOG_LEVEL_INFO};
    int loaded = extension_load(paths, count, rt, &reg, &cfg, &ext_ctx, &ectx);
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
    snprintf(p1, sizeof(p1), "%s/hooks.mjs", ext_dir);
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
    extension_load(paths, count, rt, &reg, &cfg, &ext_ctx, NULL);
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
    snprintf(p1, sizeof(p1), "%s/bad.mjs", ext_dir);
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
    int loaded = extension_load(paths, count, rt, &reg, &cfg, &ext_ctx, NULL);
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

/* T261: afterResponse hook receives response object */

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setup_mjs_env();
    printf("test_extension_api:\n");
    test_register_tool();
    test_register_hook();
    test_register_tool_invalid();
    test_hook_event_from_name();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
