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
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_DB "/tmp/cclaw_test_qjs_getconfig.db"

/* ── Parent side: registry rows → the JSON object shipped over the wire ── */

static void test_config_json_resolution(void) {
    test_db_clean(TEST_DB);
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);

    int rc = sqlite3_exec(db,
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
        "INSERT OR REPLACE INTO config(key, value) VALUES('max_iterations', '25');",
        NULL, NULL, NULL);
    assert(rc == SQLITE_OK);

    char *js = tool_extension_config_json(db, "forecast");
    assert(js);
    /* value resolved, extension prefix stripped; default_value fills in. */
    assert(strstr(js, "\"base_url\":\"https://wx.example\""));
    assert(strstr(js, "\"units\":\"metric\""));
    /* secret config never crosses into the sandbox payload */
    assert(!strstr(js, "api_key") && !strstr(js, "WEATHER_KEY"));
    /* nor does another extension's config or a global key */
    assert(!strstr(js, "nope") && !strstr(js, "max_iterations"));
    free(js);

    /* A built-in C tool owns no extension, and an unknown tool likewise:
     * both resolve to the empty object, not NULL. */
    js = tool_extension_config_json(db, "shell_exec");
    assert(js && strcmp(js, "{}") == 0);
    free(js);
    js = tool_extension_config_json(db, "no_such_tool");
    assert(js && strcmp(js, "{}") == 0);
    free(js);

    db_close(db);
    test_db_clean(TEST_DB);
}

/* ── Child side: the JS binding ── */

static void expect_eval(const char *code, const char *config_json,
                        const char *want) {
    char *r = qjs_eval_run(code, NULL, NULL, config_json, NULL);
    if (!r || strcmp(r, want) != 0)
        fprintf(stderr, "    code=%s\n    got=%s want=%s\n", code, r ? r : "NULL", want);
    assert(r && strcmp(r, want) == 0);
    free(r);
}

static void test_get_config_binding(void) {
    const char *cfg = "{\"base_url\":\"https://wx.example\",\"units\":\"metric\"}";

    expect_eval("getConfig('base_url')", cfg, "https://wx.example");
    /* Miss returns null, matching channel.getConfig. */
    expect_eval("'' + getConfig('nope')", cfg, "null");
    /* Bound even with no config at all, so handlers can probe safely. */
    expect_eval("'' + getConfig('anything')", NULL, "null");
    expect_eval("typeof getConfig", NULL, "function");
    /* Malformed config degrades to empty rather than poisoning the eval. */
    expect_eval("'' + getConfig('base_url')", "not json", "null");
}

static void test_get_config_shape(void) {
    const char *cfg = "{\"units\":\"metric\"}";

    /* Missing key argument throws, like channel.getConfig. */
    expect_eval("(function(){ try { getConfig(); return 'no throw'; }"
                " catch (e) { return 'threw'; } })()",
                cfg, "threw");
    /* The object lives in the function's closure — it is not dumped onto the
     * global scope where handler code could read or rewrite it wholesale. */
    expect_eval("typeof config", cfg, "undefined");
}

int main(void) {
    TEST_INIT();
    printf("test_qjs_getconfig:\n");
    RUN(test_config_json_resolution);
    RUN(test_get_config_binding);
    RUN(test_get_config_shape);
    return 0;
}
