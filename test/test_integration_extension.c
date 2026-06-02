/* T264/T265/T266/T267: Integration test — extensions in agent loop with mock LLM.
 * T264: extension registers tool → agent calls it via LLM tool_call
 * T265: beforeToolCall hook blocks execution → LLM receives error result
 * T266: extension throws during load → skipped, other extensions still load, agent turn continues
 * T267: callTool re-entrancy — JS→C→JS, verify depth limit enforced */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <curl/curl.h>
#include "agent.h"
#include "db.h"
#include "config.h"
#include "mock_server.h"
#include "extension.h"
#include "tool_js.h"
#include "tools.h"
#include <mquickjs.h>

static int s_port;
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

static void cleanup(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", path);
    (void)system(cmd);
}

/* Dispatch through ToolRegistry */
static char *dispatch_tools(const char *name, const char *arguments, void *user_data) {
    ToolRegistry *reg = (ToolRegistry *)user_data;
    ToolEntry *e = tools_lookup(reg, name);
    if (!e) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
        return err;
    }
    return e->handler(arguments, e->user_data);
}

/* T264: Extension registers a tool, LLM calls it, agent dispatches, final response uses result */
static void test_extension_tool_in_agent_loop(void) {
    TEST(extension_tool_in_agent_loop);

    const char *ws = "/tmp/cclaw_integ_ext_t264";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Extension that registers "add_numbers" tool */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/math.js", ext_dir);
    write_file(p1,
        "cclaw.registerTool({\n"
        "  name: 'add_numbers',\n"
        "  description: 'Add two numbers',\n"
        "  parameters: {type:'object',properties:{a:{type:'number'},b:{type:'number'}},required:['a','b']},\n"
        "  handler: function(args) { return String(args.a + args.b); }\n"
        "});\n");

    /* Set up JS runtime + extensions */
    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("js_runtime_create");

    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {0};
    cfg.log_level = LOG_LEVEL_INFO;
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);
    js_runtime_set_registry(rt, &reg);

    /* Verify tool registered */
    ToolEntry *te = tools_lookup(&reg, "add_numbers");
    if (!te) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        cleanup(ws); FAIL("add_numbers not registered");
    }

    /* Mock LLM: first returns tool_call to add_numbers, second returns final */
    mock_server_reset();
    mock_server_enqueue(200,
        "{\"id\":\"mock\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"add_numbers\",\"arguments\":\"{\\\"a\\\":3,\\\"b\\\":4}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":5,\"total_tokens\":15}}");
    mock_server_enqueue(200,
        "{\"id\":\"mock2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"The sum is 7.\"},\"finish_reason\":\"stop\"}],\"usage\":"
        "{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}}");

    /* Set up DB + session */
    sqlite3 *db = db_open(":memory:");
    if (!db) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("db_open"); }

    int64_t sid = session_create(db, "ext_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("session_create"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You are a helpful assistant.";

    /* Build tool schemas from registry */
    size_t schema_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &schema_count);

    Message user_msg = {.role = ROLE_USER, .content = "What is 3 + 4?"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = dispatch_tools;
    ctx.dispatch_data = &reg;
    ctx.tools = schemas;
    ctx.tool_count = schema_count;
    ctx.ext_ctx = &ext_ctx;

    int rc = agent_run(&ctx);
    if (rc != 0) {
        db_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("agent_run failed");
    }

    /* Verify entries: user → assistant(tool_call) → tool_result → assistant(final) */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 4) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL(msg);
    }

    /* Tool result should be "7" (from extension handler) */
    if (entries[2].message.role != ROLE_TOOL || !entries[2].message.tool_result ||
        !strstr(entries[2].message.tool_result->content, "7")) {
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("tool_result should contain '7'");
    }

    /* Final response */
    if (entries[3].message.role != ROLE_ASSISTANT || !entries[3].message.content ||
        !strstr(entries[3].message.content, "7")) {
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("final response should contain '7'");
    }

    entry_branch_free(entries, count);
    db_close(db);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* T265: beforeToolCall hook blocks execution → error result sent to LLM */
static void test_before_tool_call_blocks(void) {
    TEST(before_tool_call_blocks);

    const char *ws = "/tmp/cclaw_integ_ext_t265";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Extension: registers a tool + a hook that blocks it */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/guard.js", ext_dir);
    write_file(p1,
        "cclaw.registerTool({\n"
        "  name: 'dangerous_op',\n"
        "  description: 'A dangerous operation',\n"
        "  parameters: {type:'object',properties:{}},\n"
        "  handler: function(args) { return 'SHOULD_NOT_RUN'; }\n"
        "});\n"
        "cclaw.registerHook('beforeToolCall', function(ctx) {\n"
        "  if (ctx.name === 'dangerous_op') {\n"
        "    return {block: true, reason: 'operation not permitted'};\n"
        "  }\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("js_runtime_create");

    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {0};
    cfg.log_level = LOG_LEVEL_INFO;
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);
    js_runtime_set_registry(rt, &reg);

    /* Mock LLM: first calls dangerous_op, second gives final (acknowledging error) */
    mock_server_reset();
    mock_server_enqueue(200,
        "{\"id\":\"mock\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"dangerous_op\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":5,\"total_tokens\":15}}");
    mock_server_enqueue(200,
        "{\"id\":\"mock2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"The operation was blocked.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("db_open"); }

    int64_t sid = session_create(db, "hook_test", NULL, -1, 0);
    if (sid < 0) { db_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("session_create"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You are a helpful assistant.";

    size_t schema_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &schema_count);

    Message user_msg = {.role = ROLE_USER, .content = "Do the dangerous thing"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = dispatch_tools;
    ctx.dispatch_data = &reg;
    ctx.tools = schemas;
    ctx.tool_count = schema_count;
    ctx.ext_ctx = &ext_ctx;

    int rc = agent_run(&ctx);
    if (rc != 0) {
        db_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("agent_run failed");
    }

    /* Verify: tool_result contains error (not "SHOULD_NOT_RUN") */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 4) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL(msg);
    }

    /* Tool result should contain error from hook, not the handler output */
    if (entries[2].message.role != ROLE_TOOL || !entries[2].message.tool_result) {
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("entry[2] should be tool_result");
    }
    const char *tr = entries[2].message.tool_result->content;
    if (strstr(tr, "SHOULD_NOT_RUN") != NULL) {
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("tool handler should NOT have run");
    }
    if (strstr(tr, "error") == NULL && strstr(tr, "not permitted") == NULL) {
        printf("got: %s ", tr);
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("tool_result should contain error/not permitted");
    }

    entry_branch_free(entries, count);
    db_close(db);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* T266: Extension throws during load → skipped, other extensions still load, agent turn continues */
static void test_extension_throw_skipped_agent_continues(void) {
    TEST(extension_throw_skipped_agent_continues);

    const char *ws = "/tmp/cclaw_integ_ext_t266";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* First extension throws */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/aaa_bad.js", ext_dir);
    write_file(p1, "throw new Error('intentional load failure');");

    /* Second extension registers a tool (loads after bad due to alpha sort) */
    char p2[512];
    snprintf(p2, sizeof(p2), "%s/bbb_good.js", ext_dir);
    write_file(p2,
        "cclaw.registerTool({\n"
        "  name: 'greet',\n"
        "  description: 'Greet someone',\n"
        "  parameters: {type:'object',properties:{name:{type:'string'}},required:['name']},\n"
        "  handler: function(args) { return 'Hello, ' + args.name + '!'; }\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("js_runtime_create");

    ToolRegistry reg;
    tools_init(&reg);
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {0};
    cfg.log_level = LOG_LEVEL_INFO;
    int loaded = extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);
    js_runtime_set_registry(rt, &reg);

    /* Only 1 of 2 should have loaded */
    if (loaded != 1) {
        printf("loaded=%d ", loaded);
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("expected 1 loaded (bad skipped)");
    }

    /* greet tool must be registered */
    if (!tools_lookup(&reg, "greet")) {
        tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("greet tool not registered");
    }

    /* Mock LLM: calls greet, then final */
    mock_server_reset();
    mock_server_enqueue(200,
        "{\"id\":\"m1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"greet\",\"arguments\":\"{\\\"name\\\":\\\"World\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":5,\"total_tokens\":15}}");
    mock_server_enqueue(200,
        "{\"id\":\"m2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Hello, World!\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("db_open"); }

    int64_t sid = session_create(db, "t266", NULL, -1, 0);
    if (sid < 0) { db_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("session_create"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You are a helpful assistant.";

    size_t schema_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &schema_count);

    Message user_msg = {.role = ROLE_USER, .content = "Greet World"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = dispatch_tools;
    ctx.dispatch_data = &reg;
    ctx.tools = schemas;
    ctx.tool_count = schema_count;
    ctx.ext_ctx = &ext_ctx;

    int rc = agent_run(&ctx);
    if (rc != 0) {
        db_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("agent_run failed");
    }

    /* Verify: tool_result contains "Hello, World!" */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 4) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL(msg);
    }
    if (!entries[2].message.tool_result ||
        !strstr(entries[2].message.tool_result->content, "Hello, World!")) {
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("tool_result should contain 'Hello, World!'");
    }

    entry_branch_free(entries, count);
    db_close(db);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    cleanup(ws);
    PASS();
}

/* T267: callTool re-entrancy — JS calls C tool (echo_via_js) which invokes JS,
 * which calls back into C via callTool. Verify depth limit stops infinite recursion. */

/* C tool that invokes JS via the runtime — simulates C→JS re-entry */
static JsSessionRuntime *s_reentry_rt;

static char *reentry_handler(const char *arguments, void *user_data) {
    (void)user_data;
    (void)arguments;
    /* Execute JS that calls callTool back into C */
    if (!s_reentry_rt || !s_reentry_rt->ctx) return strdup("error: no runtime");
    JSContext *ctx = (JSContext *)s_reentry_rt->ctx;
    const char *wrapped = "(function(){ try { return __cclaw_call_tool('reentry_c', '{}'); }"
                          "catch(e) { return 'depth_caught:' + e.message; } })()";
    JSValue val = JS_Eval(ctx, wrapped, strlen(wrapped), "<reentry>", JS_EVAL_RETVAL);
    if (JS_IsException(val)) {
        return strdup("depth_exceeded");
    }
    JSCStringBuf buf;
    const char *s = JS_ToCString(ctx, val, &buf);
    return strdup(s ? s : "null");
}

static void test_calltool_reentry_depth_limit(void) {
    TEST(calltool_reentry_depth_limit);

    const char *ws = "/tmp/cclaw_integ_ext_t267";
    cleanup(ws);
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Extension that calls our C tool via callTool */
    char p1[512];
    snprintf(p1, sizeof(p1), "%s/reentry.js", ext_dir);
    write_file(p1,
        "cclaw.registerTool({\n"
        "  name: 'start_reentry',\n"
        "  description: 'Start re-entrant call',\n"
        "  parameters: {type:'object',properties:{}},\n"
        "  handler: function(args) {\n"
        "    return cclaw.callTool('reentry_c', {});\n"
        "  }\n"
        "});\n");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("js_runtime_create");
    s_reentry_rt = rt;

    ToolRegistry reg;
    tools_init(&reg);

    /* Register C tool that calls back into JS (which calls back into C...) */
    tools_register(&reg, "reentry_c", "re-entrant C tool", "{}", reentry_handler, NULL);

    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);

    size_t ext_count = 0;
    char **paths = extension_discover(ws, &ext_count);
    Config cfg = {0};
    cfg.log_level = LOG_LEVEL_INFO;
    extension_load(paths, ext_count, rt, &reg, &cfg, &ext_ctx);
    extension_list_free(paths, ext_count);
    js_runtime_set_registry(rt, &reg);

    /* Mock LLM: calls start_reentry, then final */
    mock_server_reset();
    mock_server_enqueue(200,
        "{\"id\":\"m1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"start_reentry\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,"
        "\"completion_tokens\":5,\"total_tokens\":15}}");
    mock_server_enqueue(200,
        "{\"id\":\"m2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Done.\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":5,\"total_tokens\":25}}");

    sqlite3 *db = db_open(":memory:");
    if (!db) { tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("db_open"); }

    int64_t sid = session_create(db, "t267", NULL, -1, 0);
    if (sid < 0) { db_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL("session_create"); }

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "mock-key";
    cfg.provider.model = "mock-model";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You are a helpful assistant.";

    size_t schema_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &schema_count);

    Message user_msg = {.role = ROLE_USER, .content = "Start reentry test"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = dispatch_tools;
    ctx.dispatch_data = &reg;
    ctx.tools = schemas;
    ctx.tool_count = schema_count;
    ctx.ext_ctx = &ext_ctx;

    int rc = agent_run(&ctx);
    if (rc != 0) {
        db_close(db); tools_free(&reg); extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("agent_run failed");
    }

    /* Verify: agent completed (4 entries), tool result shows depth was caught */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 4) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws); FAIL(msg);
    }

    /* Tool result must indicate depth was caught (not infinite loop / crash) */
    const char *tr = entries[2].message.tool_result ?
                     entries[2].message.tool_result->content : "";
    if (strstr(tr, "depth") == NULL && strstr(tr, "recursion") == NULL &&
        strstr(tr, "exceeded") == NULL) {
        printf("got: %s ", tr);
        entry_branch_free(entries, count); db_close(db); tools_free(&reg);
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt); cleanup(ws);
        FAIL("expected depth limit indication in tool result");
    }

    entry_branch_free(entries, count);
    db_close(db);
    tools_free(&reg);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    s_reentry_rt = NULL;
    cleanup(ws);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_port = mock_server_start();
    if (s_port < 0) {
        fprintf(stderr, "Failed to start mock server\n");
        return 1;
    }
    printf("--- test_integration_extension (T264/T265/T266/T267) ---\n");
    test_extension_tool_in_agent_loop();
    test_before_tool_call_blocks();
    test_extension_throw_skipped_agent_continues();
    test_calltool_reentry_depth_limit();
    printf("%d/%d passed\n", tests_passed, tests_run);
    mock_server_stop();
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
