/* Hook dispatch — fresh QuickJS context per invocation.
 * Hooks now load from the DB `hooks` table (extension_load_hooks); the dispatch
 * logic (beforeRequest chaining, gating, observer) is unchanged. Each handler
 * file holds a function expression. */
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
#include "qjs_helpers.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

#define STORE "/tmp/cclaw_hookstore"

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

/* Seed one hook into the DB + shared store, then load it into ext_ctx.
 * agent owns the extension (so the load join's visibility test passes). */
static void seed_hook(sqlite3 *db, const char *agent, const char *ext,
                      const char *event, const char *fn_source) {
    char dir[512], path[700], sql[4096];
    snprintf(dir, sizeof(dir), "%s/%s", STORE, ext);
    mkdirs(dir);
    snprintf(path, sizeof(path), "%s/%s.qjs", dir, event);
    write_file(path, fn_source);

    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO extensions(name,path,owner_agent,published,enabled) "
        "VALUES('%s','%s','%s',0,1);"
        "INSERT OR REPLACE INTO agent_extensions(agent_name,extension_name,enabled) "
        "VALUES('%s','%s',1);"
        "INSERT OR REPLACE INTO hooks(extension_name,event,path,enabled) "
        "VALUES('%s','%s','%s',1);",
        ext, dir, agent, agent, ext, ext, event, path);
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "seed_hook sql error: %s\n", err ? err : "?");
        sqlite3_free(err);
    }
}

/* No hooks loaded → returns NULL */
static void test_no_hooks(void) {
    TEST(no_hooks_returns_null);

    ExtensionCtx ext_ctx;
    JsSessionRuntime *rt = js_runtime_create();
    extension_ctx_init(&ext_ctx, rt);

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

/* Hook modifies messages — prepends a system message */
static void test_hook_modifies_messages(void) {
    TEST(hook_modifies_messages);
    cleanup(STORE);

    const char *db_path = "/tmp/cclaw_hook_t1.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db open");

    seed_hook(db, "test", "inject", "beforeRequest",
        "(function(msgs){ msgs.unshift({role:'system',content:'injected by hook'}); return msgs; })");

    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("rt create");
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);
    extension_load_hooks(&ext_ctx, db, "test");

    if (ext_ctx.hooks[HOOK_BEFORE_REQUEST].count != 1) {
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        sqlite3_close(db); cleanup(STORE); unlink(db_path); FAIL("expected 1 hook");
    }

    Config cfg = {.log_level = LOG_LEVEL_INFO,
                  .provider = {.model = "test-model", .max_tokens = 100}};
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append_with_turn(db, sid, &user_msg, 1);

    ContextPlan plan;
    if (context_plan(db, sid, &cfg, 0, &plan) != 0) {
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        sqlite3_close(db); cleanup(STORE); unlink(db_path); FAIL("context_plan");
    }

    char *body = hook_dispatch_before_request(&ext_ctx, db, sid, &cfg, &plan, NULL, 0);
    context_plan_free(&plan);

    int ok = body && strstr(body, "injected by hook") && strstr(body, "hello") &&
             strstr(body, "test-model");
    if (!ok && body) printf("body: %.200s ", body);
    free(body);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    sqlite3_close(db);
    cleanup(STORE);
    unlink(db_path);
    if (!ok) FAIL("body missing expected content");
    PASS();
}

/* Hook throws → skipped, messages pass through unmodified */
static void test_hook_throws(void) {
    TEST(hook_throws_skipped);
    cleanup(STORE);

    const char *db_path = "/tmp/cclaw_hook_t2.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);

    seed_hook(db, "test", "bad", "beforeRequest",
        "(function(msgs){ throw new Error('oops'); })");

    JsSessionRuntime *rt = js_runtime_create();
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);
    extension_load_hooks(&ext_ctx, db, "test");

    Config cfg = {.log_level = LOG_LEVEL_INFO,
                  .provider = {.model = "test-model", .max_tokens = 50}};
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "hi"};
    entry_append_with_turn(db, sid, &user_msg, 1);

    ContextPlan plan;
    context_plan(db, sid, &cfg, 0, &plan);
    char *body = hook_dispatch_before_request(&ext_ctx, db, sid, &cfg, &plan, NULL, 0);
    context_plan_free(&plan);

    int ok = body && strstr(body, "hi");
    free(body);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    sqlite3_close(db);
    cleanup(STORE);
    unlink(db_path);
    if (!ok) FAIL("expected body with original msg even on throw");
    PASS();
}

/* Multiple hooks chain in load order (ordered by extension_name) */
static void test_hooks_chain(void) {
    TEST(hooks_chain_in_order);
    cleanup(STORE);

    const char *db_path = "/tmp/cclaw_hook_t3.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);

    seed_hook(db, "test", "a_first", "beforeRequest",
        "(function(msgs){ msgs.push({role:'system',content:'FIRST'}); return msgs; })");
    seed_hook(db, "test", "b_second", "beforeRequest",
        "(function(msgs){ msgs.push({role:'system',content:'SECOND'}); return msgs; })");

    JsSessionRuntime *rt = js_runtime_create();
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);
    extension_load_hooks(&ext_ctx, db, "test");

    if (ext_ctx.hooks[HOOK_BEFORE_REQUEST].count != 2) {
        extension_ctx_destroy(&ext_ctx); js_runtime_destroy(rt);
        sqlite3_close(db); cleanup(STORE); unlink(db_path); FAIL("expected 2 hooks");
    }

    Config cfg = {.log_level = LOG_LEVEL_INFO, .provider = {.model = "test-model"}};
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "test"};
    entry_append_with_turn(db, sid, &user_msg, 1);

    ContextPlan plan;
    context_plan(db, sid, &cfg, 0, &plan);
    char *body = hook_dispatch_before_request(&ext_ctx, db, sid, &cfg, &plan, NULL, 0);
    context_plan_free(&plan);

    char *first_pos = body ? strstr(body, "FIRST") : NULL;
    char *second_pos = body ? strstr(body, "SECOND") : NULL;
    int ok = first_pos && second_pos && first_pos < second_pos;
    if (!ok && body) printf("body: %.300s ", body);
    free(body);
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    sqlite3_close(db);
    cleanup(STORE);
    unlink(db_path);
    if (!ok) FAIL("hooks not chained in order");
    PASS();
}

/* §8 gating hook: restrict-only {allow|ask|deny}, most-restrictive wins. */
static void test_gate_restrict_only(void) {
    TEST(gate_restrict_only);
    cleanup(STORE);

    const char *db_path = "/tmp/cclaw_hook_t4.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);

    seed_hook(db, "test", "gate", "beforeToolCall",
        "(function(ctx){"
        "  if (ctx.name === 'dangerous') return {deny:true, reason:'not allowed'};"
        "  if (ctx.name === 'risky') return {ask:true, reason:'confirm please'};"
        "  if (ctx.name === 'legacy') return {block:true};"
        "  if (ctx.name === 'greedy') return {allow:true};"
        "})");

    JsSessionRuntime *rt = js_runtime_create();
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);
    extension_load_hooks(&ext_ctx, db, "test");

    char *reason = NULL;
    HookGate g;
    int ok = 1;

    g = hook_dispatch_gate_tool_call(&ext_ctx, db, "dangerous", "{}", &reason);
    if (g != HOOK_GATE_DENY || !reason || strcmp(reason, "not allowed") != 0) ok = 0;
    free(reason); reason = NULL;

    g = hook_dispatch_gate_tool_call(&ext_ctx, db, "risky", "{}", &reason);
    if (g != HOOK_GATE_ASK || !reason || strcmp(reason, "confirm please") != 0) ok = 0;
    free(reason); reason = NULL;

    if (hook_dispatch_gate_tool_call(&ext_ctx, db, "legacy", "{}", NULL) != HOOK_GATE_DENY) ok = 0;
    if (hook_dispatch_gate_tool_call(&ext_ctx, db, "greedy", "{}", NULL) != HOOK_GATE_ALLOW) ok = 0;
    if (hook_dispatch_gate_tool_call(&ext_ctx, db, "safe", "{}", NULL) != HOOK_GATE_ALLOW) ok = 0;

    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    sqlite3_close(db);
    cleanup(STORE);
    unlink(db_path);
    if (!ok) FAIL("gate decisions incorrect");
    PASS();
}

/* §8 observer hook: side-effect only — runs, cannot modify the result. */
static void test_observer_side_effect_only(void) {
    TEST(observer_side_effect_only);
    cleanup(STORE);

    const char *db_path = "/tmp/cclaw_hook_t5.db";
    unlink(db_path);
    sqlite3 *db = test_db_open(db_path);

    seed_hook(db, "test", "observe", "afterToolCall",
        "(function(ctx){ if (!ctx.name || !ctx.result) throw new Error('bad context');"
        "  return {result:'SHOULD BE IGNORED'}; })");

    JsSessionRuntime *rt = js_runtime_create();
    ExtensionCtx ext_ctx;
    extension_ctx_init(&ext_ctx, rt);
    extension_load_hooks(&ext_ctx, db, "test");

    hook_dispatch_observe_tool_call(&ext_ctx, db, "fetch", "{}", "original data");

    int ok = ext_ctx.hooks[HOOK_AFTER_TOOL_CALL].count > 0;
    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    sqlite3_close(db);
    cleanup(STORE);
    unlink(db_path);
    if (!ok) FAIL("observer hook not loaded");
    PASS();
}

/* No tool-call hooks → gate ALLOWs, observer is a no-op. */
static void test_no_tool_hooks(void) {
    TEST(no_tool_hooks);
    ExtensionCtx ext_ctx;
    JsSessionRuntime *rt = js_runtime_create();
    extension_ctx_init(&ext_ctx, rt);
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);

    char *reason = (char *)0x1;  /* must be NULLed even when no hooks */
    HookGate g = hook_dispatch_gate_tool_call(&ext_ctx, db, "x", "{}", &reason);
    int ok = (g == HOOK_GATE_ALLOW && reason == NULL);
    hook_dispatch_observe_tool_call(&ext_ctx, db, "x", "{}", "r");  /* no crash */

    extension_ctx_destroy(&ext_ctx);
    js_runtime_destroy(rt);
    sqlite3_close(db);
    if (!ok) FAIL("expected ALLOW + NULL reason with no hooks");
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_hook_dispatch:\n");
    test_no_hooks();
    test_hook_modifies_messages();
    test_hook_throws();
    test_hooks_chain();
    test_gate_restrict_only();
    test_observer_side_effect_only();
    test_no_tool_hooks();
    cleanup(STORE);
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
