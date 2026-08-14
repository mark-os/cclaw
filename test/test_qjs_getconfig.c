/* test_qjs_getconfig.c — getConfig() on the tool JS surface (Q4).
 *
 * Two halves, because the feature spans the sandbox boundary: the parent
 * resolves an extension's config out of the registry (tool_extension_config_json),
 * and the child — which has no DB — exposes that object to the handler as
 * getConfig(key). Both are tested in-process; no fork, no namespace. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "qjs_helpers.h"
#include "test_util.h"
#include "tools.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DB "/tmp/cclaw_test_qjs_getconfig.db"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql failed: %s\n", err ? err : "?");
        sqlite3_free(err);
        abort();
    }
}

/* ── Parent side: registry rows → the JSON object shipped over the wire ── */

static void test_config_json_resolution(void) {
    TEST(config_json_resolution);

    test_db_clean(TEST_DB);
    sqlite3 *db = test_db_open(TEST_DB);
    if (!db) FAIL("db_open");

    exec_sql(db,
        "INSERT INTO extensions(name, path) VALUES('weather', '/tmp/x');"
        "INSERT INTO tools(name, extension_name, path)"
        "  VALUES('forecast', 'weather', '/tmp/x/forecast.qjs');"
        "INSERT INTO tools(name, path) VALUES('shell_exec', NULL);"
        /* value overrides default_value; a default-only row still resolves. */
        "INSERT INTO config(key, value, default_value)"
        "  VALUES('weather.base_url', 'https://wx.example', 'https://unused');"
        "INSERT INTO config(key, value, default_value)"
        "  VALUES('weather.units', NULL, 'metric');"
        /* secret rows never cross into the sandbox... */
        "INSERT INTO config(key, value, secret)"
        "  VALUES('weather.api_key', 'WEATHER_KEY', 1);"
        /* ...and neither does another extension's config, nor a global key. */
        "INSERT INTO config(key, value) VALUES('otherext.base_url', 'nope');"
        "INSERT OR REPLACE INTO config(key, value) VALUES('max_iterations', '25');");

    char *js = tool_extension_config_json(db, "forecast");
    if (!js) { db_close(db); test_db_clean(TEST_DB); FAIL("NULL json"); }
    if (!strstr(js, "\"base_url\":\"https://wx.example\"")) {
        free(js); db_close(db); test_db_clean(TEST_DB);
        FAIL("value not resolved / prefix not stripped");
    }
    if (!strstr(js, "\"units\":\"metric\"")) {
        free(js); db_close(db); test_db_clean(TEST_DB);
        FAIL("default_value not resolved");
    }
    if (strstr(js, "api_key") || strstr(js, "WEATHER_KEY")) {
        free(js); db_close(db); test_db_clean(TEST_DB);
        FAIL("secret config leaked into the sandbox payload");
    }
    if (strstr(js, "nope") || strstr(js, "max_iterations")) {
        free(js); db_close(db); test_db_clean(TEST_DB);
        FAIL("foreign config keys leaked");
    }
    free(js);

    /* A built-in C tool owns no extension: empty object, not NULL. */
    js = tool_extension_config_json(db, "shell_exec");
    if (!js || strcmp(js, "{}") != 0) {
        free(js); db_close(db); test_db_clean(TEST_DB);
        FAIL("built-in tool should resolve to {}");
    }
    free(js);

    /* An unknown tool likewise resolves to the empty object. */
    js = tool_extension_config_json(db, "no_such_tool");
    if (!js || strcmp(js, "{}") != 0) {
        free(js); db_close(db); test_db_clean(TEST_DB);
        FAIL("unknown tool should resolve to {}");
    }
    free(js);

    db_close(db);
    test_db_clean(TEST_DB);
    PASS();
}

/* ── Child side: the JS binding ── */

static void expect_eval(const char *code, const char *config_json,
                        const char *want, const char **err) {
    char *r = qjs_eval_run(code, NULL, NULL, config_json, NULL);
    if (!r) { *err = "qjs_eval_run returned NULL"; return; }
    if (strcmp(r, want) != 0) {
        fprintf(stderr, "\n    code=%s\n    got=%s want=%s\n", code, r, want);
        *err = "unexpected eval result";
    }
    free(r);
}

static void test_get_config_binding(void) {
    TEST(get_config_binding);
    const char *err = NULL;
    const char *cfg = "{\"base_url\":\"https://wx.example\",\"units\":\"metric\"}";

    /* Hit returns the string. */
    expect_eval("getConfig('base_url')", cfg, "https://wx.example", &err);
    if (err) FAIL(err);

    /* Miss returns null, matching channel.getConfig. */
    expect_eval("'' + getConfig('nope')", cfg, "null", &err);
    if (err) FAIL(err);

    /* Bound even with no config at all, so handlers can probe safely. */
    expect_eval("'' + getConfig('anything')", NULL, "null", &err);
    if (err) FAIL(err);
    expect_eval("typeof getConfig", NULL, "function", &err);
    if (err) FAIL(err);

    /* Malformed config degrades to empty rather than poisoning the eval. */
    expect_eval("'' + getConfig('base_url')", "not json", "null", &err);
    if (err) FAIL(err);

    PASS();
}

static void test_get_config_shape(void) {
    TEST(get_config_shape);
    const char *err = NULL;
    const char *cfg = "{\"units\":\"metric\"}";

    /* Missing key argument throws, like channel.getConfig. */
    expect_eval("(function(){ try { getConfig(); return 'no throw'; }"
                " catch (e) { return 'threw'; } })()",
                cfg, "threw", &err);
    if (err) FAIL(err);

    /* The object lives in the function's closure — it is not dumped onto the
     * global scope where handler code could read or rewrite it wholesale. */
    expect_eval("typeof config", cfg, "undefined", &err);
    if (err) FAIL(err);

    PASS();
}

int main(void) {
    TEST_INIT();
    printf("test_qjs_getconfig:\n");
    test_config_json_resolution();
    test_get_config_binding();
    test_get_config_shape();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
