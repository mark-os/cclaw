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
    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test");

    ToolRegistry reg;
    tools_init(&reg);
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg};
    tool_js_define_register(&reg, &ctx);

    char *r = tool_js_define_handler(
        "{\"name\":\"add\",\"description\":\"Add two numbers\","
        "\"parameters\":\"{}\","
        "\"code\":\"return String(args.a + args.b)\"}",
        &ctx);
    if (!r || strstr(r, "defined") == NULL) { FAIL(r ? r : "NULL"); free(r); tools_free(&reg); db_close(db); return; }
    free(r);

    /* Tool should be registered */
    ToolEntry *e = tools_lookup(&reg, "add");
    if (!e) { FAIL("not registered"); tools_free(&reg); db_close(db); return; }

    /* Call it */
    char *result = e->handler("{\"a\":3,\"b\":4}", e->user_data);
    if (!result || strcmp(result, "7") != 0) { FAIL(result ? result : "NULL"); free(result); tools_free(&reg); db_close(db); return; }
    free(result);

    tools_free(&reg);
    db_close(db);
    PASS();
}

static void test_define_persist_reload(void) {
    TEST("define_persist_reload");
    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test");

    ToolRegistry reg;
    tools_init(&reg);
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg};
    tool_js_define_register(&reg, &ctx);

    /* Define a tool */
    char *r = tool_js_define_handler(
        "{\"name\":\"greet\",\"code\":\"return 'hi ' + args.name\"}",
        &ctx);
    free(r);

    /* Simulate reload: new registry, load from DB */
    ToolRegistry reg2;
    tools_init(&reg2);
    int loaded = tool_js_load_session(db, sid, &reg2);
    if (loaded != 1) { FAIL("load count != 1"); tools_free(&reg); tools_free(&reg2); db_close(db); return; }

    ToolEntry *e = tools_lookup(&reg2, "greet");
    if (!e) { FAIL("not found after reload"); tools_free(&reg); tools_free(&reg2); db_close(db); return; }

    char *result = e->handler("{\"name\":\"world\"}", e->user_data);
    if (!result || strcmp(result, "hi world") != 0) { FAIL(result ? result : "NULL"); free(result); tools_free(&reg); tools_free(&reg2); db_close(db); return; }
    free(result);

    tools_free(&reg);
    tools_free(&reg2);
    db_close(db);
    PASS();
}

static void test_define_missing_name(void) {
    TEST("define_missing_name");
    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test");

    ToolRegistry reg;
    tools_init(&reg);
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg};

    char *r = tool_js_define_handler("{\"code\":\"return 1\"}", &ctx);
    if (!r || strncmp(r, "error:", 6) != 0) { FAIL(r ? r : "NULL"); free(r); tools_free(&reg); db_close(db); return; }
    free(r);

    tools_free(&reg);
    db_close(db);
    PASS();
}

static void test_define_redefine(void) {
    TEST("define_redefine");
    sqlite3 *db = db_open(":memory:");
    if (!db) { FAIL("db_open"); return; }
    int64_t sid = session_create(db, "test");

    ToolRegistry reg;
    tools_init(&reg);
    JsDefineCtx ctx = {.db = db, .session_id = sid, .reg = &reg};
    tool_js_define_register(&reg, &ctx);

    /* Define v1 */
    char *r = tool_js_define_handler("{\"name\":\"calc\",\"code\":\"return '1'\"}", &ctx);
    free(r);

    /* Redefine v2 */
    r = tool_js_define_handler("{\"name\":\"calc\",\"code\":\"return '2'\"}", &ctx);
    free(r);

    ToolEntry *e = tools_lookup(&reg, "calc");
    if (!e) { FAIL("not found"); tools_free(&reg); db_close(db); return; }
    char *result = e->handler("{}", e->user_data);
    if (!result || strcmp(result, "2") != 0) { FAIL(result ? result : "NULL"); free(result); tools_free(&reg); db_close(db); return; }
    free(result);

    tools_free(&reg);
    db_close(db);
    PASS();
}

static void test_register(void) {
    TEST("register");
    ToolRegistry reg;
    tools_init(&reg);
    JsDefineCtx ctx = {.db = NULL, .session_id = 0, .reg = &reg};
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
    test_define_missing_name();
    test_define_redefine();
    test_register();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
