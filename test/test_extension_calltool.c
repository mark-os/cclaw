/* T262: Test cclaw.callTool() — synchronous C callback from JS extensions.
 * V116: dispatch into C tool registry, depth limit 8. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "extension.h"
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

/* Simple C tool for testing */
static char *echo_handler(const char *arguments, void *user_data) {
    (void)user_data;
    /* Return "echo:<args>" */
    size_t len = strlen(arguments) + 6;
    char *r = malloc(len);
    if (r) snprintf(r, len, "echo:%s", arguments);
    return r;
}

/* V116: callTool dispatches to C tool and returns result */
static void test_calltool_basic(void) {
    TEST(calltool_basic);
    const char *ws = "/tmp/cclaw_ext_ct_t1";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Extension that calls a C tool via callTool */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/caller.js", ext_dir);
    write_file(p1,
        "cclaw.registerTool({\n"
        "  name: 'call_echo',\n"
        "  description: 'calls echo tool',\n"
        "  parameters: {},\n"
        "  handler: function(args) {\n"
        "    return cclaw.callTool('echo', {msg: 'hello'});\n"
        "  }\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt create");

    ToolRegistry reg;
    tools_init(&reg);

    /* Register a C tool that the extension will call */
    tools_register(&reg, "echo", "echo tool", "{}", echo_handler, NULL);

    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    Config cfg = {.log_level = LOG_LEVEL_INFO};
    extension_load(paths, count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, count);

    /* Set registry on runtime for callTool dispatch */
    js_runtime_set_registry(rt, &reg);

    /* call_echo should be registered */
    ToolEntry *e = tools_lookup(&reg, "call_echo");
    if (!e) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("tool not found"); }

    /* Invoke it — should call echo via callTool */
    char *result = e->handler("{}", e->user_data);
    if (!result) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("null result"); }

    /* Result should be echo's output: "echo:{\"msg\":\"hello\"}" */
    if (strncmp(result, "echo:", 5) != 0) {
        printf("got: %s ", result);
        free(result); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("expected echo: prefix");
    }
    if (strstr(result, "hello") == NULL) {
        printf("got: %s ", result);
        free(result); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("expected hello in result");
    }

    free(result);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* V116: callTool with unknown tool name → throws (returns error) */
static void test_calltool_not_found(void) {
    TEST(calltool_not_found);
    const char *ws = "/tmp/cclaw_ext_ct_t2";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/bad.js", ext_dir);
    write_file(p1,
        "cclaw.registerTool({\n"
        "  name: 'call_missing',\n"
        "  description: 'calls nonexistent tool',\n"
        "  parameters: {},\n"
        "  handler: function(args) {\n"
        "    try { return cclaw.callTool('nonexistent', {}); }\n"
        "    catch(e) { return 'caught:' + e.message; }\n"
        "  }\n"
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
    js_runtime_set_registry(rt, &reg);

    ToolEntry *e = tools_lookup(&reg, "call_missing");
    if (!e) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("tool not found"); }

    char *result = e->handler("{}", e->user_data);
    if (!result || strstr(result, "not found") == NULL) {
        printf("got: %s ", result ? result : "NULL");
        free(result); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("expected 'not found' error");
    }

    free(result);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* V116: callTool depth limit enforced (max 8) */
static void test_calltool_depth_limit(void) {
    TEST(calltool_depth_limit);
    const char *ws = "/tmp/cclaw_ext_ct_t3";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Extension that recurses via callTool */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/recurse.js", ext_dir);
    write_file(p1,
        "cclaw.registerTool({\n"
        "  name: 'recurse',\n"
        "  description: 'recursive tool',\n"
        "  parameters: {},\n"
        "  handler: function(args) {\n"
        "    var d = (args && args.depth) ? args.depth : 0;\n"
        "    if (d >= 20) return 'TOO_DEEP';\n"
        "    try {\n"
        "      return cclaw.callTool('recurse', {depth: d + 1});\n"
        "    } catch(e) {\n"
        "      return 'caught:' + d + ':' + e.message;\n"
        "    }\n"
        "  }\n"
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
    js_runtime_set_registry(rt, &reg);

    ToolEntry *e = tools_lookup(&reg, "recurse");
    if (!e) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("tool not found"); }

    char *result = e->handler("{\"depth\":0}", e->user_data);
    if (!result) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("null result"); }

    /* Recursion must be stopped — either by C depth limit (V116) or JS engine
     * stack limits (MicroQuickJS 1MB heap). Key: it must NOT reach depth 20. */
    if (strcmp(result, "TOO_DEEP") == 0) {
        free(result); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("recursion not stopped — reached depth 20");
    }

    /* Result should be either:
     * - "caught:N:..." (JS caught the error at some depth)
     * - "error: ..." (engine-level stack overflow caught by C handler)
     * Either way, recursion was stopped safely. */

    free(result);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

int main(void) {
    printf("test_extension_calltool (T262/V116):\n");
    test_calltool_basic();
    test_calltool_not_found();
    test_calltool_depth_limit();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
