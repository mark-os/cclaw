#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tool_js.h"
#include "tools.h"
#include "db.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_define_basic(void) {
    TEST("define_basic");
    sqlite3 *db = test_db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolRegistry reg;
    tools_init(&reg);
    JsSessionRuntime *rt = js_runtime_create();
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg, .rt = rt};
    tool_js_define_register(&reg, &ctx);

    char *r = tool_js_define_handler(
        "{\"name\":\"add\",\"description\":\"Add two numbers\","
        "\"parameters\":\"{}\","
        "\"code\":\"return String(args.a + args.b)\"}",
        &ctx);
    if (!r || strstr(r, "defined") == NULL) { FAIL(r ? r : "NULL"); free(r); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }
    free(r);

    /* Tool should be registered */
    ToolEntry *e = tools_lookup(&reg, "add");
    if (!e) { FAIL("not registered"); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }

    /* Call it */
    char *result = e->handler("{\"a\":3,\"b\":4}", e->user_data);
    if (!result || strcmp(result, "7") != 0) { FAIL(result ? result : "NULL"); free(result); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }
    free(result);

    js_runtime_destroy(rt);
    tools_free(&reg);
    db_close(db);
    PASS();
}

static void test_define_persist_reload(void) {
    TEST("define_persist_reload");
    sqlite3 *db = test_db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolRegistry reg;
    tools_init(&reg);
    JsSessionRuntime *rt = js_runtime_create();
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg, .rt = rt};
    tool_js_define_register(&reg, &ctx);

    /* Define a tool */
    char *r = tool_js_define_handler(
        "{\"name\":\"greet\",\"code\":\"return 'hi ' + args.name\"}",
        &ctx);
    free(r);
    js_runtime_destroy(rt);

    /* Simulate reload: new registry + new runtime, load from DB */
    ToolRegistry reg2;
    tools_init(&reg2);
    JsSessionRuntime *rt2 = js_runtime_create();
    int loaded = tool_js_load_session(db, sid, &reg2, rt2);
    if (loaded != 1) { FAIL("load count != 1"); js_runtime_destroy(rt2); tools_free(&reg); tools_free(&reg2); db_close(db); return; }

    ToolEntry *e = tools_lookup(&reg2, "greet");
    if (!e) { FAIL("not found after reload"); js_runtime_destroy(rt2); tools_free(&reg); tools_free(&reg2); db_close(db); return; }

    char *result = e->handler("{\"name\":\"world\"}", e->user_data);
    if (!result || strcmp(result, "hi world") != 0) { FAIL(result ? result : "NULL"); free(result); js_runtime_destroy(rt2); tools_free(&reg); tools_free(&reg2); db_close(db); return; }
    free(result);

    js_runtime_destroy(rt2);
    tools_free(&reg);
    tools_free(&reg2);
    db_close(db);
    PASS();
}

/* T33: Verify shared context — tool B can call helper defined by tool A after reload */
static void test_context_replay_shared_state(void) {
    TEST("context_replay_shared_state");
    sqlite3 *db = test_db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolRegistry reg;
    tools_init(&reg);
    JsSessionRuntime *rt = js_runtime_create();
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg, .rt = rt};
    tool_js_define_register(&reg, &ctx);

    /* Define tool A: registers a helper function in global scope */
    char *r = tool_js_define_handler(
        "{\"name\":\"setup_helper\",\"code\":\"globalThis.dbl = function(x){return x*2}; return 'ok'\"}",
        &ctx);
    if (!r || strstr(r, "defined") == NULL) { FAIL(r ? r : "NULL"); free(r); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }
    free(r);

    /* Define tool B: uses the helper from tool A */
    r = tool_js_define_handler(
        "{\"name\":\"use_helper\",\"code\":\"return String(dbl(args.n))\"}",
        &ctx);
    if (!r || strstr(r, "defined") == NULL) { FAIL(r ? r : "NULL"); free(r); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }
    free(r);

    /* Verify tool B works in current session */
    ToolEntry *e = tools_lookup(&reg, "use_helper");
    if (!e) { FAIL("use_helper not registered"); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }
    char *result = e->handler("{\"n\":5}", e->user_data);
    if (!result || strcmp(result, "10") != 0) { FAIL(result ? result : "NULL"); free(result); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }
    free(result);

    /* Destroy and reload — simulates session resume */
    js_runtime_destroy(rt);
    tools_free(&reg);

    ToolRegistry reg2;
    tools_init(&reg2);
    JsSessionRuntime *rt2 = js_runtime_create();
    int loaded = tool_js_load_session(db, sid, &reg2, rt2);
    if (loaded != 2) { FAIL("expected 2 tools loaded"); js_runtime_destroy(rt2); tools_free(&reg2); db_close(db); return; }

    /* After replay, tool B should still work because tool A's code
     * re-registered globalThis.dbl in the shared runtime */
    e = tools_lookup(&reg2, "use_helper");
    if (!e) { FAIL("use_helper not found after reload"); js_runtime_destroy(rt2); tools_free(&reg2); db_close(db); return; }
    result = e->handler("{\"n\":7}", e->user_data);
    if (!result || strcmp(result, "14") != 0) { FAIL(result ? result : "NULL"); free(result); js_runtime_destroy(rt2); tools_free(&reg2); db_close(db); return; }
    free(result);

    js_runtime_destroy(rt2);
    tools_free(&reg2);
    db_close(db);
    PASS();
}

static void test_define_missing_name(void) {
    TEST("define_missing_name");
    sqlite3 *db = test_db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolRegistry reg;
    tools_init(&reg);
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg, .rt = NULL};

    char *r = tool_js_define_handler("{\"code\":\"return 1\"}", &ctx);
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); tools_free(&reg); db_close(db); return; }
    free(r);

    tools_free(&reg);
    db_close(db);
    PASS();
}

static void test_define_redefine(void) {
    TEST("define_redefine");
    sqlite3 *db = test_db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolRegistry reg;
    tools_init(&reg);
    JsSessionRuntime *rt = js_runtime_create();
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg, .rt = rt};
    tool_js_define_register(&reg, &ctx);

    /* Define v1 */
    char *r = tool_js_define_handler("{\"name\":\"calc\",\"code\":\"return '1'\"}", &ctx);
    free(r);

    /* Redefine v2 */
    r = tool_js_define_handler("{\"name\":\"calc\",\"code\":\"return '2'\"}", &ctx);
    free(r);

    ToolEntry *e = tools_lookup(&reg, "calc");
    if (!e) { FAIL("not found"); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }
    char *result = e->handler("{}", e->user_data);
    if (!result || strcmp(result, "2") != 0) { FAIL(result ? result : "NULL"); free(result); js_runtime_destroy(rt); tools_free(&reg); db_close(db); return; }
    free(result);

    js_runtime_destroy(rt);
    tools_free(&reg);
    db_close(db);
    PASS();
}

static void test_register(void) {
    TEST("register");
    ToolRegistry reg;
    tools_init(&reg);
    JsDefineCtx ctx = {.db = NULL, .session_id = 0, .reg = &reg, .rt = NULL};
    int rc = tool_js_define_register(&reg, &ctx);
    if (rc != 0) { FAIL("register failed"); tools_free(&reg); return; }
    ToolEntry *e = tools_lookup(&reg, "js_define_tool");
    if (!e) { FAIL("lookup failed"); tools_free(&reg); return; }
    tools_free(&reg);
    PASS();
}

int main(void) {
    printf("test_js_define:\n");
    test_define_basic();
    test_define_persist_reload();
    test_context_replay_shared_state();
    test_define_missing_name();
    test_define_redefine();
    test_register();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
