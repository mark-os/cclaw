#define _POSIX_C_SOURCE 200809L
#include "extension_manifest.h"
#include "db.h"
#include "test_util.h"
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
    "  ]\n"
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

    /* shared-store copy exists */
    struct stat sb;
    assert(stat(STORE "/forecast.qjs", &sb) == 0 && S_ISREG(sb.st_mode));
    assert(stat(STORE "/guard.qjs", &sb) == 0);

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

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_extension_manifest:\n");
    test_install_ingests_rows();
    test_reinstall_is_idempotent();
    test_validate_rejects_missing_handler();
    test_validate_rejects_bad_json();
    /* cleanup */
    unlink(TEST_DB);
    rm_rf("/tmp/test_ext_manifest_bundle");
    rm_rf("/tmp/extensions");
    printf("test_extension_manifest: ALL PASS\n");
    return 0;
}
