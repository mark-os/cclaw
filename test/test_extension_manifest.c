#define _POSIX_C_SOURCE 200809L
#include "extension_manifest.h"
#include "config_registry.h"
#include "db.h"
#include "test_util.h"
#include "tool_extension.h"
#include "tools.h"
#include "util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_extension_manifest.sqlite"
#define BUNDLE  "/tmp/test_ext_manifest_bundle/nws"
#define STORE   "/tmp/extensions/nws"   /* <dirname(TEST_DB)>/extensions/nws */

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void rm_rf(const char *path) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int rc = system(cmd);
    (void)rc;
}

static const char *MANIFEST =
    "{\n"
    "  \"name\": \"nws\",\n"
    "  \"version\": \"0.1.0\",\n"
    "  \"description\": \"US National Weather Service tools\",\n"
    "  \"tools\": [\n"
    "    { \"name\": \"get_forecast\",\n"
    "      \"description\": \"Get the NWS forecast for a lat/lon\",\n"
    "      \"parameters\": {\"type\":\"object\",\"properties\":{\"lat\":{\"type\":\"number\"},\"lon\":{\"type\":\"number\"}},\"required\":[\"lat\",\"lon\"]},\n"
    "      \"handler\": \"forecast.qjs\" }\n"
    "  ],\n"
    "  \"hooks\": [\n"
    "    { \"event\": \"beforeToolCall\", \"handler\": \"guard.qjs\" }\n"
    "  ],\n"
    "  \"scripts\": [\n"
    "    { \"name\": \"morning_report\", \"handler\": \"report.qjs\", \"schedule\": \"0 7 * * *\" }\n"
    "  ],\n"
    "  \"config\": [\n"
    "    { \"key\": \"api_base\", \"default\": \"https://api.weather.gov\",\n"
    "      \"description\": \"NWS API base URL\" },\n"
    "    { \"key\": \"units\", \"default\": \"us\", \"description\": \"Unit system\" }\n"
    "  ],\n"
    "  \"skills\": [ \"skills/forecast-style\", \"notes.md\" ]\n"
    "}\n";

/* Query a single text cell. Caller frees. */
static char *q1(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    char *res = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            if (v) res = strdup(v);
        }
        sqlite3_finalize(st);
    }
    return res;
}

static int qint(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    int res = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) res = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    return res;
}

static void setup_bundle(void) {
    rm_rf("/tmp/test_ext_manifest_bundle");
    rm_rf("/tmp/extensions");
    mkdir("/tmp/test_ext_manifest_bundle", 0755);
    mkdir(BUNDLE, 0755);
    write_file(BUNDLE "/extension.json", MANIFEST);
    write_file(BUNDLE "/forecast.qjs", "http_request('https://api.weather.gov/').body");
    write_file(BUNDLE "/guard.qjs", "(function(c){ return c.name==='shell_exec' ? {block:true} : undefined; })");
    write_file(BUNDLE "/report.qjs", "'report'");
    mkdir(BUNDLE "/skills", 0755);
    mkdir(BUNDLE "/skills/forecast-style", 0755);
    write_file(BUNDLE "/skills/forecast-style/SKILL.md",
               "---\ndescription: How to format forecast output\n---\nbody\n");
    write_file(BUNDLE "/notes.md",
               "---\ndescription: Note-taking conventions\n---\nbody\n");
}

static void test_install_ingests_rows(void) {
    unlink(TEST_DB);
    setup_bundle();
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);

    char *err = NULL;
    int rc = extension_install(db, BUNDLE, "default", &err);
    if (rc != 0) fprintf(stderr, "install error: %s\n", err ? err : "?");
    assert(rc == 0);
    assert(err == NULL);

    /* extensions row */
    assert(qint(db, "SELECT count(*) FROM extensions WHERE name='nws'") == 1);
    char *owner = q1(db, "SELECT owner_agent FROM extensions WHERE name='nws'");
    assert(owner && strcmp(owner, "default") == 0); free(owner);
    assert(qint(db, "SELECT published FROM extensions WHERE name='nws'") == 0);
    char *ver = q1(db, "SELECT version FROM extensions WHERE name='nws'");
    assert(ver && strcmp(ver, "0.1.0") == 0); free(ver);

    /* tools row */
    assert(qint(db, "SELECT count(*) FROM tools WHERE name='get_forecast'") == 1);
    char *en = q1(db, "SELECT extension_name FROM tools WHERE name='get_forecast'");
    assert(en && strcmp(en, "nws") == 0); free(en);
    assert(qint(db, "SELECT builtin FROM tools WHERE name='get_forecast'") == 0);
    char *path = q1(db, "SELECT path FROM tools WHERE name='get_forecast'");
    assert(path && strstr(path, "/extensions/nws/forecast.qjs")); free(path);
    char *params = q1(db, "SELECT parameters_json FROM tools WHERE name='get_forecast'");
    assert(params && strstr(params, "\"lat\"")); free(params);

    /* hooks row */
    assert(qint(db, "SELECT count(*) FROM hooks WHERE extension_name='nws' AND event='beforeToolCall'") == 1);
    char *hpath = q1(db, "SELECT path FROM hooks WHERE extension_name='nws'");
    assert(hpath && strstr(hpath, "/extensions/nws/guard.qjs")); free(hpath);

    /* agent_extensions row */
    assert(qint(db, "SELECT count(*) FROM agent_extensions WHERE agent_name='default' AND extension_name='nws'") == 1);

    /* cron seeded for scheduled script */
    assert(qint(db, "SELECT count(*) FROM cron_jobs WHERE name='morning_report'") == 1);

    /* config rows: namespaced <ext>.<key>, code-owned default + description */
    assert(qint(db, "SELECT count(*) FROM config WHERE key LIKE 'nws.%'") == 2);
    char *cdef = q1(db, "SELECT default_value FROM config WHERE key='nws.api_base'");
    assert(cdef && strcmp(cdef, "https://api.weather.gov") == 0); free(cdef);
    char *cdesc = q1(db, "SELECT description FROM config WHERE key='nws.units'");
    assert(cdesc && strcmp(cdesc, "Unit system") == 0); free(cdesc);


    /* shared-store copy exists */
    struct stat sb;
    assert(stat(STORE "/forecast.qjs", &sb) == 0 && S_ISREG(sb.st_mode));
    assert(stat(STORE "/guard.qjs", &sb) == 0);
    /* skills: copied into the store, no DB state */
    assert(stat(STORE "/skills/forecast-style/SKILL.md", &sb) == 0);
    assert(stat(STORE "/notes.md", &sb) == 0);

    db_close(db);
    printf("  PASS test_install_ingests_rows\n");
}

static void test_reinstall_is_idempotent(void) {
    unlink(TEST_DB);
    setup_bundle();
    sqlite3 *db = test_db_open(TEST_DB);
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    /* simulate a publish, then re-install: published must survive */
    sqlite3_exec(db, "UPDATE extensions SET published=1 WHERE name='nws'", NULL, NULL, NULL);
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    assert(qint(db, "SELECT published FROM extensions WHERE name='nws'") == 1);
    /* no duplicate tool rows */
    assert(qint(db, "SELECT count(*) FROM tools WHERE name='get_forecast'") == 1);
    db_close(db);
    printf("  PASS test_reinstall_is_idempotent\n");
}

static void test_config_lifecycle(void) {
    unlink(TEST_DB);
    setup_bundle();
    sqlite3 *db = test_db_open(TEST_DB);
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", &err) == 0);

    /* config_set accepts an extension-registered key... */
    assert(config_set(db, "nws.units", "metric") == 0);
    char *v = config_get(db, "nws.units");
    assert(v && strcmp(v, "metric") == 0); free(v);
    /* ...and still rejects anonymous keys */
    assert(config_set(db, "nws.nosuch", "x") == -1);
    assert(config_set(db, "rogue.key", "x") == -1);

    /* re-install: override preserved, defaults refreshed */
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    v = config_get(db, "nws.units");
    assert(v && strcmp(v, "metric") == 0); free(v);

    /* a key dropped from the manifest is deleted on re-install */
    char patched[4096];
    snprintf(patched, sizeof(patched), "%s", MANIFEST);
    char *cut = strstr(patched, ",\n    { \"key\": \"units\"");
    assert(cut);
    char *end = strchr(cut + 1, '}');
    assert(end);
    memmove(cut, end + 1, strlen(end + 1) + 1);
    write_file(BUNDLE "/extension.json", patched);
    assert(extension_install(db, BUNDLE, "default", &err) == 0);
    assert(qint(db, "SELECT count(*) FROM config WHERE key='nws.units'") == 0);
    assert(qint(db, "SELECT count(*) FROM config WHERE key='nws.api_base'") == 1);

    db_close(db);
    printf("  PASS test_config_lifecycle\n");
}

static void test_validate_rejects_bad_sections(void) {
    rm_rf("/tmp/test_ext_badsec");
    mkdir("/tmp/test_ext_badsec", 0755);
    char *err = NULL;

    /* config key with a dot (namespace escape) */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"config\":[{\"key\":\"a.b\",\"description\":\"d\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    free(err); err = NULL;

    /* config entry without a description */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"config\":[{\"key\":\"ok\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    free(err); err = NULL;

    /* skill path escaping the bundle */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"skills\":[\"../evil\"]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    free(err); err = NULL;

    /* declared skill missing its SKILL.md */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"skills\":[\"skills/ghost\"]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    free(err); err = NULL;

    rm_rf("/tmp/test_ext_badsec");
    printf("  PASS test_validate_rejects_bad_sections\n");
}

static void test_validate_rejects_missing_handler(void) {
    rm_rf("/tmp/test_ext_bad");
    mkdir("/tmp/test_ext_bad", 0755);
    mkdir("/tmp/test_ext_bad/bad", 0755);
    /* manifest references forecast.qjs but the file is absent */
    write_file("/tmp/test_ext_bad/bad/extension.json",
               "{\"name\":\"bad\",\"tools\":[{\"name\":\"t\",\"handler\":\"forecast.qjs\"}]}");
    char *err = NULL;
    int rc = extension_manifest_validate("/tmp/test_ext_bad/bad", &err);
    assert(rc == -1);
    assert(err != NULL);
    free(err);
    rm_rf("/tmp/test_ext_bad");
    printf("  PASS test_validate_rejects_missing_handler\n");
}

static void test_validate_rejects_bad_json(void) {
    rm_rf("/tmp/test_ext_badjson");
    mkdir("/tmp/test_ext_badjson", 0755);
    write_file("/tmp/test_ext_badjson/extension.json", "{ not json ]");
    char *err = NULL;
    assert(extension_manifest_validate("/tmp/test_ext_badjson", &err) == -1);
    free(err);
    rm_rf("/tmp/test_ext_badjson");
    printf("  PASS test_validate_rejects_bad_json\n");
}

/* extension_fork: a manifest-less builtin bundle (like telegram's) forks into
 * a workspace draft with a synthesized, renamed manifest that then passes a
 * real extension_install — the full fork → edit → promote roundtrip. */
static void test_extension_fork_roundtrip(void) {
    unlink(TEST_DB);
    rm_rf("/tmp/test_fork_src");
    rm_rf("/tmp/test_fork_ws");
    rm_rf("/tmp/extensions");
    sqlite3 *db = test_db_open(TEST_DB);

    /* Fake builtin: handler + locale json, no extension.json — exactly how
     * extract_builtin_extensions registers the telegram bundle. */
    mkdir("/tmp/test_fork_src", 0755);
    write_file("/tmp/test_fork_src/channel.qjs", "function onInit() { return {}; }\n");
    write_file("/tmp/test_fork_src/tg.json", "{\"messages\":{}}\n");
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name, path, builtin) VALUES('tg','/tmp/test_fork_src',1);"
        "INSERT INTO channels(name, extension_name, type, binary_path, status)"
        " VALUES('tg','tg','telegram','/tmp/test_fork_src/channel.qjs','active');",
        NULL, NULL, NULL) == SQLITE_OK);

    ToolRegistry reg;
    tools_init(&reg);
    ToolExtensionCtx ctx = { .db = db, .agent_name = "default",
                             .workspace = "/tmp/test_fork_ws" };
    assert(tool_extension_register(&reg, &ctx) == 0);
    ToolEntry *t = tools_lookup(&reg, "extension_fork");
    assert(t);

    char *r = t->handler("{\"name\":\"tg\",\"as\":\"tg-ng\"}", t->user_data);
    assert(r && strstr(r, "forked extension 'tg'"));
    free(r);

    /* Draft materialized: handler copied, manifest synthesized + renamed */
    struct stat st;
    assert(stat("/tmp/test_fork_ws/extensions/tg-ng/channel.qjs", &st) == 0);
    size_t mlen = 0;
    char *manifest = util_read_file("/tmp/test_fork_ws/extensions/tg-ng/extension.json", &mlen);
    assert(manifest);
    assert(strstr(manifest, "\"tg-ng\""));
    assert(strstr(manifest, "\"channel.qjs\""));
    assert(strstr(manifest, "\"telegram\""));   /* channel type recovered */
    free(manifest);

    /* The synthesized manifest passes a real promote */
    char *err = NULL;
    assert(extension_install(db, "/tmp/test_fork_ws/extensions/tg-ng", "default", &err) == 0);
    free(err);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT status FROM channels WHERE name='tg-ng';", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "draft") == 0);
    sqlite3_finalize(s);

    /* Refusals: existing draft, unknown source, invisible source */
    r = t->handler("{\"name\":\"tg\",\"as\":\"tg-ng\"}", t->user_data);
    assert(r && strstr(r, "already exists")); free(r);
    r = t->handler("{\"name\":\"nope\"}", t->user_data);
    assert(r && strstr(r, "not registered")); free(r);
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name, path, builtin, published, owner_agent)"
        " VALUES('private','/tmp/test_fork_src',0,0,'someone-else');",
        NULL, NULL, NULL) == SQLITE_OK);
    r = t->handler("{\"name\":\"private\"}", t->user_data);
    assert(r && strstr(r, "not visible")); free(r);

    tools_free(&reg);
    db_close(db);
    rm_rf("/tmp/test_fork_src");
    rm_rf("/tmp/test_fork_ws");
    printf("  PASS test_extension_fork_roundtrip\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_extension_manifest:\n");
    test_install_ingests_rows();
    test_reinstall_is_idempotent();
    test_config_lifecycle();
    test_validate_rejects_missing_handler();
    test_validate_rejects_bad_json();
    test_validate_rejects_bad_sections();
    test_extension_fork_roundtrip();
    /* cleanup */
    unlink(TEST_DB);
    rm_rf("/tmp/test_ext_manifest_bundle");
    rm_rf("/tmp/extensions");
    printf("test_extension_manifest: ALL PASS\n");
    return 0;
}
