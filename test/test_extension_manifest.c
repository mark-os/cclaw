#define _POSIX_C_SOURCE 200809L
#include "extension_manifest.h"
#include "agent_define.h"
#include "hook.h"
#include "channel.h"
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

/* channel.o (channel_should_launch) references resolve_approval — the stub
 * keeps resolve.o (and the dispatch/loop tree behind it) out of the link. */
#include "resolve.h"
void resolve_approval(int64_t approval_id, ApprovalDecision decision,
                      const char *decided_via, int64_t grant_expires_at) {
    (void)approval_id; (void)decision; (void)decided_via; (void)grant_expires_at;
}

/* Make exactly one registry read fail while every other query on the same
 * connection still works — the shape a locked/corrupt-DB moment takes from
 * channel_should_launch's point of view, and the only way to isolate one of
 * its four reads from the others (they all hit the config table). */
static int deny_config_reads(void *ud, int action, const char *a1, const char *a2,
                             const char *a3, const char *a4) {
    (void)ud; (void)a2; (void)a3; (void)a4;
    if (action == SQLITE_READ && a1 && strcmp(a1, "config") == 0) return SQLITE_DENY;
    return SQLITE_OK;
}

/* Narrower: only the value column, which is what config_get() selects and the
 * required-key list query does not. */
static int deny_config_value(void *ud, int action, const char *a1, const char *a2,
                             const char *a3, const char *a4) {
    (void)ud; (void)a3; (void)a4;
    if (action == SQLITE_READ && a1 && a2 &&
        strcmp(a1, "config") == 0 && strcmp(a2, "value") == 0) return SQLITE_DENY;
    return SQLITE_OK;
}

#define TEST_DB "/tmp/test_extension_manifest.sqlite"
#define BUNDLE  "/tmp/test_ext_manifest_bundle/nws"
#define STORE   "/tmp/extensions/nws"   /* <dirname(TEST_DB)>/extensions/nws */

/* extension_install writes agent_extensions/grants/cron_jobs rows keyed by
 * the owner's agent_name — an enforced FK (v31). Seed every owner name this
 * file installs as before any child rows are written. */
static sqlite3 *open_seeded(void) {
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    test_seed_agent(db, "default");
    test_seed_agent(db, "grantee");
    test_seed_agent(db, "intruder");
    /* No 'system' agent: a builtin install has no owner at all (v59). */
    return db;
}

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
    write_file(BUNDLE "/report.qjs", "'report'");
    mkdir(BUNDLE "/skills", 0755);
    mkdir(BUNDLE "/skills/forecast-style", 0755);
    write_file(BUNDLE "/skills/forecast-style/SKILL.md",
               "---\ndescription: How to format forecast output\n---\nbody\n");
    write_file(BUNDLE "/notes.md",
               "---\ndescription: Note-taking conventions\n---\nbody\n");
}

static void test_install_ingests_rows(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = open_seeded();
    assert(db);

    char *err = NULL;
    int rc = extension_install(db, BUNDLE, "default", 0, &err);
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
    char *path = q1(db, "SELECT path FROM tools WHERE name='get_forecast'");
    assert(path && strstr(path, "/extensions/nws/forecast.qjs")); free(path);
    char *params = q1(db, "SELECT parameters_json FROM tools WHERE name='get_forecast'");
    assert(params && strstr(params, "\"lat\"")); free(params);

    /* agent_extensions row */
    assert(qint(db, "SELECT count(*) FROM agent_extensions WHERE agent_name='default' AND extension_name='nws'") == 1);

    /* cron seeded for scheduled script: a `script` payload pointing at the
     * store handler — the fire runs it sandboxed, with no LLM turn (it used to
     * seed a prompt telling the model to call js_eval). */
    assert(qint(db, "SELECT count(*) FROM cron_jobs WHERE name='morning_report'") == 1);
    char *cscript = q1(db, "SELECT script FROM cron_jobs WHERE name='morning_report'");
    assert(cscript && strstr(cscript, "/extensions/nws/report.qjs")); free(cscript);
    char *ctask = q1(db, "SELECT task FROM cron_jobs WHERE name='morning_report'");
    assert(ctask && ctask[0] == '\0'); free(ctask);

    /* config rows: namespaced <ext>.<key>, code-owned default + description */
    assert(qint(db, "SELECT count(*) FROM config WHERE key LIKE 'nws.%'") == 2);
    char *cdef = q1(db, "SELECT default_value FROM config WHERE key='nws.api_base'");
    assert(cdef && strcmp(cdef, "https://api.weather.gov") == 0); free(cdef);
    char *cdesc = q1(db, "SELECT description FROM config WHERE key='nws.units'");
    assert(cdesc && strcmp(cdesc, "Unit system") == 0); free(cdesc);


    /* shared-store copy exists */
    struct stat sb;
    assert(stat(STORE "/forecast.qjs", &sb) == 0 && S_ISREG(sb.st_mode));
    /* skills: copied into the store, no DB state */
    assert(stat(STORE "/skills/forecast-style/SKILL.md", &sb) == 0);
    assert(stat(STORE "/notes.md", &sb) == 0);

    db_close(db);
    printf("  PASS test_install_ingests_rows\n");
}

/* $.tools[].droppable: a tool says its calls may be taken out of context by
 * a turn review. Unstated means protected; anything but a boolean is refused. */
static void test_tool_droppable(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = open_seeded();
    assert(db);
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    assert(qint(db, "SELECT droppable FROM tools WHERE name='get_forecast'") == 0);

    static const char *FMT =
        "{\"name\":\"nws\",\"tools\":[{\"name\":\"get_forecast\",\"description\":\"d\","
        "\"parameters\":{\"type\":\"object\"},\"handler\":\"forecast.qjs\","
        "\"droppable\":%s}]}";
    char m[512];
    static const char *BAD[] = { "1", "\"yes\"", "null", "[true]" };
    for (size_t i = 0; i < sizeof(BAD) / sizeof(BAD[0]); i++) {
        snprintf(m, sizeof(m), FMT, BAD[i]);
        write_file(BUNDLE "/extension.json", m);
        assert(extension_manifest_validate(BUNDLE, &err) == -1);
        assert(err && strstr(err, "'droppable' must be true or false"));
        free(err); err = NULL;
    }
    snprintf(m, sizeof(m), FMT, "true");
    write_file(BUNDLE "/extension.json", m);
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    assert(qint(db, "SELECT droppable FROM tools WHERE name='get_forecast'") == 1);
    snprintf(m, sizeof(m), FMT, "false");
    write_file(BUNDLE "/extension.json", m);
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    assert(qint(db, "SELECT droppable FROM tools WHERE name='get_forecast'") == 0);

    db_close(db);
    printf("  PASS test_tool_droppable\n");
}

/* $.hooks[]: each entry becomes a hooks row pointing into the store, waited
 * for unless it says "async": true, with its place in the manifest and its
 * own timeout, and a re-promote replaces the rows — the failure count starts
 * over with the new code, and a hook its failures disabled is enabled again. */
static void test_hook_ingest(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = open_seeded();
    assert(db);
    char *err = NULL;
    write_file(BUNDLE "/review.qjs", "return '{}';");
    write_file(BUNDLE "/observe.qjs", "return '{}';");
    write_file(BUNDLE "/extension.json",
        "{\"name\":\"nws\",\"hooks\":["
        "{\"event\":\"Stop\",\"handler\":\"review.qjs\",\"timeout\":90,"
        " \"input\":{\"history\":3, \"max_bytes\":2000}},"
        "{\"event\":\"Stop\",\"handler\":\"observe.qjs\",\"async\":true},"
        "{\"event\":\"ContextThreshold\",\"handler\":\"review.qjs\",\"async\":false}]}");
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    assert(qint(db, "SELECT count(*) FROM hooks WHERE extension_name='nws'"
                    " AND event='Stop' AND enabled=1 AND fail_count=0 AND disabled=0") == 2);
    assert(qint(db, "SELECT async FROM hooks WHERE path='" STORE "/review.qjs'"
                    " AND event='Stop'") == 0);
    assert(qint(db, "SELECT async FROM hooks WHERE path='" STORE "/observe.qjs'") == 1);
    assert(qint(db, "SELECT async FROM hooks WHERE event='ContextThreshold'") == 0);
    assert(qint(db, "SELECT group_concat(pos, ',') = '0,1,2' FROM (SELECT pos FROM hooks"
                    " WHERE extension_name='nws' ORDER BY pos)") == 1);
    assert(qint(db, "SELECT timeout FROM hooks WHERE event='Stop' AND path='" STORE
                    "/review.qjs'") == 90);
    assert(qint(db, "SELECT timeout IS NULL FROM hooks WHERE path='" STORE "/observe.qjs'") == 1);
    /* `input` is stored as declared; a hook that declares none has NULL —
     * the runtime's defaults. */
    assert(qint(db, "SELECT input IS NULL FROM hooks WHERE path='" STORE "/observe.qjs'") == 1);
    assert(qint(db, "SELECT input = '{\"history\":3,\"max_bytes\":2000}'"
                    " FROM hooks WHERE path='" STORE "/review.qjs'") == 1);

    sqlite3_exec(db, "UPDATE hooks SET fail_count=7, disabled=1", NULL, NULL, NULL);
    write_file(BUNDLE "/extension.json",
        "{\"name\":\"nws\",\"hooks\":["
        "{\"event\":\"Stop\",\"handler\":\"review.qjs\",\"async\":false}]}");
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    assert(qint(db, "SELECT count(*) FROM hooks WHERE extension_name='nws'") == 1);
    assert(qint(db, "SELECT async*100 + fail_count*10 + disabled FROM hooks"
                    " WHERE extension_name='nws'") == 0);

    db_close(db);
    printf("  PASS test_hook_ingest\n");
}

static void test_reinstall_is_idempotent(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = open_seeded();
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    /* simulate a publish, then re-install: published must survive */
    sqlite3_exec(db, "UPDATE extensions SET published=1 WHERE name='nws'", NULL, NULL, NULL);
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    assert(qint(db, "SELECT published FROM extensions WHERE name='nws'") == 1);
    /* no duplicate tool rows */
    assert(qint(db, "SELECT count(*) FROM tools WHERE name='get_forecast'") == 1);
    db_close(db);
    printf("  PASS test_reinstall_is_idempotent\n");
}

static void test_config_lifecycle(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = open_seeded();
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);

    /* config_set accepts an extension-registered key... */
    assert(config_set(db, "nws.units", "metric") == 0);
    char *v = config_get(db, "nws.units");
    assert(v && strcmp(v, "metric") == 0); free(v);
    /* ...and still rejects anonymous keys */
    assert(config_set(db, "nws.nosuch", "x") == -1);
    assert(config_set(db, "rogue.key", "x") == -1);

    /* re-install: override preserved, defaults refreshed */
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
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
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
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

    /* The events: Stop, SubagentStop, ContextThreshold. Any other name is
     * refused: a registered hook that never fires is a silent no-op. Names
     * this build knows of but does not fire yet are refused by name, with
     * the reason. An empty section is fine. */
    write_file("/tmp/test_ext_badsec/h.qjs", "({})");
    static const struct { const char *event, *why; } bad_events[] = {
        { "turnSettled",   "is not an event" },
        { "stop",          "is not an event" },
        { "preAdvance",    "is not an event" },
        { "bogusEvent",    "is not an event" },
        { "PreToolUse",    "not available until stage 2" },
        { "PostToolUse",   "not available until stage 2" } };
    for (size_t i = 0; i < sizeof(bad_events) / sizeof(bad_events[0]); i++) {
        char mf[160];
        snprintf(mf, sizeof(mf), "{\"name\":\"b\",\"hooks\":[{\"event\":\"%s\","
                 "\"handler\":\"h.qjs\"}]}", bad_events[i].event);
        write_file("/tmp/test_ext_badsec/extension.json", mf);
        assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
        if (!err || !strstr(err, bad_events[i].why) || !strstr(err, bad_events[i].event)) {
            fprintf(stderr, "FAIL event %s: %s\n", bad_events[i].event, err ? err : "(none)");
            abort();
        }
        free(err); err = NULL;
    }
    /* An entry with no event at all, and a bad one hiding behind a good one. */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"hooks\":[{\"handler\":\"h.qjs\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "needs \"event\""));
    free(err); err = NULL;
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"hooks\":[{\"event\":\"Stop\",\"handler\":\"h.qjs\"},"
        "{\"event\":\"preAdvance\",\"handler\":\"h.qjs\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "preAdvance"));
    free(err); err = NULL;
    write_file("/tmp/test_ext_badsec/extension.json", "{\"name\":\"b\",\"hooks\":[]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == 0);
    free(err); err = NULL;

    /* Each event is accepted, with or without `async` and `timeout`. The
     * handler is checked like any other. */
    static const char *const good_hooks[] = {
        "\"event\":\"Stop\"", "\"event\":\"SubagentStop\"",
        "\"event\":\"ContextThreshold\"",
        "\"event\":\"Stop\",\"async\":true", "\"event\":\"Stop\",\"async\":false",
        "\"event\":\"ContextThreshold\",\"timeout\":1",
        "\"event\":\"Stop\",\"timeout\":3600,\"async\":true,\"input\":{}" };
    for (size_t i = 0; i < sizeof(good_hooks) / sizeof(good_hooks[0]); i++) {
        char mf[240];
        snprintf(mf, sizeof(mf), "{\"name\":\"b\",\"hooks\":[{%s,\"handler\":\"h.qjs\"}]}",
                 good_hooks[i]);
        write_file("/tmp/test_ext_badsec/extension.json", mf);
        if (extension_manifest_validate("/tmp/test_ext_badsec", &err) != 0) {
            fprintf(stderr, "FAIL hook {%s} refused: %s\n", good_hooks[i], err ? err : "?");
            abort();
        }
        free(err); err = NULL;
    }
    /* The fields a hook does not take, by name: `wait` is gone (async is its
     * inverse), `matcher` waits for stage 2, anything else is unknown; and
     * the two optional ones are checked for type and range. */
    static const struct { const char *fields, *why; } bad_hooks[] = {
        { "\"wait\":true",      "\"wait\" is gone" },
        { "\"wait\":false",     "\"async\": true" },
        { "\"matcher\":\"Bash\"", "\"matcher\" is not available until stage 2" },
        { "\"when\":\"always\"",  "unknown key \"when\"" },
        { "\"may\":[\"edit\"]",    "unknown key \"may\"" },
        { "\"async\":1",        "\"async\" must be true or false" },
        { "\"async\":\"true\"", "\"async\" must be true or false" },
        { "\"async\":null",     "\"async\" must be true or false" },
        { "\"timeout\":0",      "\"timeout\" must be a whole number of seconds from 1 to 3600" },
        { "\"timeout\":3601",   "from 1 to 3600" },
        { "\"timeout\":1.5",    "\"timeout\" must be" },
        { "\"timeout\":\"60\"", "\"timeout\" must be" } };
    for (size_t i = 0; i < sizeof(bad_hooks) / sizeof(bad_hooks[0]); i++) {
        char mf[240];
        snprintf(mf, sizeof(mf), "{\"name\":\"b\",\"hooks\":[{\"event\":\"Stop\","
                 "\"handler\":\"h.qjs\",%s}]}", bad_hooks[i].fields);
        write_file("/tmp/test_ext_badsec/extension.json", mf);
        if (extension_manifest_validate("/tmp/test_ext_badsec", &err) != -1 ||
            !err || !strstr(err, bad_hooks[i].why)) {
            fprintf(stderr, "FAIL hook {%s}: got \"%s\", want \"%s\"\n", bad_hooks[i].fields,
                    err ? err : "(none)", bad_hooks[i].why);
            abort();
        }
        free(err); err = NULL;
    }
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"hooks\":[\"Stop\"]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    free(err); err = NULL;
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"hooks\":[{\"event\":\"Stop\",\"handler\":\"nope.qjs\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    free(err); err = NULL;

    /* `input` — what a hook's script asks to be sent. Each option alone
     * and all together are accepted; anything else is refused, by name, and
     * max_bytes stops at the most the hook vehicle carries. */
    static const char *const good_input[] = {
        "{}", "{\"history\":\"none\"}", "{\"history\":\"all\"}", "{\"history\":1}",
        "{\"history\":5,\"history_results\":false,\"max_bytes\":2000000}",
        "{\"history_results\":true}", "{\"max_bytes\":16777216}", "{\"max_bytes\":1024}" };
    for (size_t i = 0; i < sizeof(good_input) / sizeof(good_input[0]); i++) {
        char mf[300];
        snprintf(mf, sizeof(mf), "{\"name\":\"b\",\"hooks\":[{\"event\":\"Stop\","
                 "\"handler\":\"h.qjs\",\"input\":%s}]}", good_input[i]);
        write_file("/tmp/test_ext_badsec/extension.json", mf);
        if (extension_manifest_validate("/tmp/test_ext_badsec", &err) != 0) {
            fprintf(stderr, "FAIL input %s refused: %s\n", good_input[i], err ? err : "?");
            abort();
        }
        free(err); err = NULL;
    }
    static const struct { const char *input, *why; } bad_input[] = {
        { "[]", "hook h.qjs: \"input\" must be an object" },
        { "null", "\"input\" must be an object" },
        { "\"all\"", "\"input\" must be an object" },
        { "{\"histroy\":\"all\"}", "unknown key \"histroy\" in \"input\"" },
        { "{\"history\":\"all\",\"extra\":1}", "unknown key \"extra\"" },
        { "{\"history\":0}", "input.history must be \"none\", \"all\" or a positive number" },
        { "{\"history\":-3}", "input.history must be" },
        { "{\"history\":2.5}", "input.history must be" },
        { "{\"history\":\"5\"}", "input.history must be" },
        { "{\"history\":\"some\"}", "input.history must be" },
        { "{\"history\":true}", "input.history must be" },
        { "{\"history_results\":0}", "input.history_results must be true or false" },
        { "{\"history_results\":\"no\"}", "input.history_results must be true or false" },
        { "{\"max_bytes\":0}", "input.max_bytes must be a whole number of bytes from 1024 to 16777216" },
        { "{\"max_bytes\":1023}", "from 1024 to 16777216" },
        { "{\"max_bytes\":16777217}", "from 1024 to 16777216" },
        { "{\"max_bytes\":2e6}", "input.max_bytes must be" },
        { "{\"max_bytes\":\"1MB\"}", "input.max_bytes must be" },
    };
    for (size_t i = 0; i < sizeof(bad_input) / sizeof(bad_input[0]); i++) {
        char mf[300];
        snprintf(mf, sizeof(mf), "{\"name\":\"b\",\"hooks\":[{\"event\":\"Stop\","
                 "\"handler\":\"h.qjs\"},{\"event\":\"Stop\",\"handler\":\"h.qjs\","
                 "\"input\":%s}]}", bad_input[i].input);
        write_file("/tmp/test_ext_badsec/extension.json", mf);
        if (extension_manifest_validate("/tmp/test_ext_badsec", &err) != -1 ||
            !err || !strstr(err, bad_input[i].why)) {
            fprintf(stderr, "FAIL input %s: got \"%s\", want \"%s\"\n", bad_input[i].input,
                    err ? err : "(none)", bad_input[i].why);
            abort();
        }
        free(err); err = NULL;
    }

    /* Tool policies are shape-checked at promote: policy_eval fails closed
     * (POLICY_ERROR) on these at runtime, so a bad policy must be rejected
     * here — not shipped as a tool bricked on first call. */
    /* policy as a string */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"policy\":\"deny all\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "invalid policy"));
    free(err); err = NULL;
    /* rules as object, not array (silent-ALLOW typo pre-fix) */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"policy\":{\"rules\":{\"match\":{},\"effect\":\"deny\"}}}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "invalid policy"));
    free(err); err = NULL;
    /* rule missing its match object */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"policy\":{\"rules\":[{\"effect\":\"deny\"}]}}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "invalid policy"));
    free(err); err = NULL;
    /* unknown effect */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"policy\":{\"rules\":[{\"match\":{},\"effect\":\"denny\"}]}}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "invalid policy"));
    free(err); err = NULL;
    /* non-text array element in a match value */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"policy\":{\"rules\":[{\"match\":{\"a\":[\"x\",7]},\"effect\":\"deny\"}]}}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "invalid policy"));
    free(err); err = NULL;
    /* a well-formed policy passes */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"policy\":{\"rules\":[{\"match\":{\"action\":[\"rm\",\"mv\"]},\"effect\":\"deny\"},"
        "{\"match\":{\"path\":\"*\"},\"effect\":\"ask\"},"
        "{\"match\":{},\"effect\":\"allow\"}]}}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == 0);
    free(err); err = NULL;

    /* channel.transports must be spelled exactly: its only consumer
     * string-matches 'persistent', so a typo would read as "no persistent
     * transport declared" and skip the WS-capability gate — failing open. */
    write_file("/tmp/test_ext_badsec/channel.qjs", "function onInit(){return {};}\n");
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"channel\":{\"type\":\"c\",\"handler\":\"channel.qjs\","
        "\"transports\":[\"persistant\"]}}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "unknown channel transport"));
    free(err); err = NULL;
    /* a bare string instead of an array makes json_each error out, which
     * reads as "no rows" in validator and consumer alike — the same
     * fail-open by shape instead of spelling. */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"channel\":{\"type\":\"c\",\"handler\":\"channel.qjs\","
        "\"transports\":\"persistent\"}}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "transports must be an array"));
    free(err); err = NULL;
    /* the three known transports pass */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"channel\":{\"type\":\"c\",\"handler\":\"channel.qjs\","
        "\"transports\":[\"poll\",\"webhook\",\"persistent\"]}}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == 0);
    free(err); err = NULL;

    /* $.channel itself must be an object. Every check inside reaches in with
     * json_extract, which answers NULL for a scalar — so "channel": 3 skipped
     * the handler check, the transports enum, and the shape assert alike, and
     * the whole section passed unexamined. */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"channel\":3}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "'$.channel' must be an object"));
    free(err); err = NULL;
    /* A string is worse: json_extract errors on it rather than returning NULL,
     * which the old code read identically to "nothing declared". */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"channel\":\"channel.qjs\"}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "must be an object"));
    free(err); err = NULL;
    /* An array is not an object either. */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"channel\":[{\"handler\":\"channel.qjs\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "must be an object"));
    free(err); err = NULL;
    /* A manifest with no channel at all is still fine — the section is optional. */
    write_file("/tmp/test_ext_badsec/extension.json", "{\"name\":\"b\"}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == 0);
    free(err); err = NULL;

    /* A manifest may name egress hosts in a default, but may not ship '*' —
     * that is match-any, and a default applies with no operator write, so a
     * bundle could unpin its own egress behind a "1 config key" summary. */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"config\":[{\"key\":\"egress_hosts\",\"description\":\"d\","
        "\"default\":\"api.example.com, *\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == -1);
    assert(err && strstr(err, "egress_hosts"));
    free(err); err = NULL;
    /* naming hosts (including a suffix rule) is fine */
    write_file("/tmp/test_ext_badsec/extension.json",
        "{\"name\":\"b\",\"config\":[{\"key\":\"egress_hosts\",\"description\":\"d\","
        "\"default\":\"gateway.discord.gg,.discord.gg\"}]}");
    assert(extension_manifest_validate("/tmp/test_ext_badsec", &err) == 0);
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

/* extension_fork: a system-owned extension (like telegram's) forks into a
 * workspace draft with its manifest renamed, that then passes a real
 * extension_install under a different name — the full fork → edit → promote
 * roundtrip. */
static void test_extension_fork_roundtrip(void) {
    test_db_clean(TEST_DB);
    rm_rf("/tmp/test_fork_src");
    rm_rf("/tmp/test_fork_ws");
    rm_rf("/tmp/extensions");
    sqlite3 *db = open_seeded();

    /* System-owned extension: handler + real extension.json — exactly how
     * extension_install_builtin registers the telegram bundle. */
    mkdir("/tmp/test_fork_src", 0755);
    write_file("/tmp/test_fork_src/channel.qjs", "function onInit() { return {}; }\n");
    write_file("/tmp/test_fork_src/extension.json",
        "{\"name\":\"tg\",\"version\":\"0.1.0\","
        "\"channel\":{\"type\":\"telegram\",\"handler\":\"channel.qjs\"}}\n");
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name, path, owner_agent, published)"
        " VALUES('tg','/tmp/test_fork_src',NULL,1);"
        "INSERT INTO channels(name, extension_name, type, status)"
        " VALUES('tg','tg','telegram','active');",
        NULL, NULL, NULL) == SQLITE_OK);

    ToolRegistry reg;
    tools_init(&reg);
    ToolExtensionCtx ctx = { .db = db, .agent_name = "default",
                             .workspace = "/tmp/test_fork_ws" };
    assert(tool_extension_register(&reg, &ctx) == 0);
    ToolEntry *t = tools_lookup(&reg, "extension_fork");
    assert(t);

    char *r = t->handler("{\"name\":\"tg\",\"as\":\"tg-ng\"}", t->user_data, &(int){0});
    assert(r && strstr(r, "forked extension 'tg'"));
    free(r);

    /* Draft materialized: handler copied, manifest renamed */
    struct stat st;
    assert(stat("/tmp/test_fork_ws/extensions/tg-ng/channel.qjs", &st) == 0);
    size_t mlen = 0;
    char *manifest = util_read_file("/tmp/test_fork_ws/extensions/tg-ng/extension.json", &mlen);
    assert(manifest);
    assert(strstr(manifest, "\"tg-ng\""));
    assert(strstr(manifest, "\"channel.qjs\""));
    assert(strstr(manifest, "\"telegram\""));
    free(manifest);

    /* The renamed manifest passes a real promote, under a new owner */
    char *err = NULL;
    assert(extension_install(db, "/tmp/test_fork_ws/extensions/tg-ng", "default", 0, &err) == 0);
    free(err);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT status FROM channels WHERE name='tg-ng';", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "draft") == 0);
    sqlite3_finalize(s);

    /* Refusals: existing draft, unknown source, invisible source */
    r = t->handler("{\"name\":\"tg\",\"as\":\"tg-ng\"}", t->user_data, &(int){0});
    assert(r && strstr(r, "already exists")); free(r);
    r = t->handler("{\"name\":\"nope\"}", t->user_data, &(int){0});
    assert(r && strstr(r, "not registered")); free(r);
    assert(sqlite3_exec(db,
        "INSERT OR IGNORE INTO agents(name) VALUES('someone-else');"
        "INSERT INTO extensions(name, path, published, owner_agent)"
        " VALUES('private','/tmp/test_fork_src',0,'someone-else');",
        NULL, NULL, NULL) == SQLITE_OK);
    r = t->handler("{\"name\":\"private\"}", t->user_data, &(int){0});
    assert(r && strstr(r, "not visible")); free(r);

    tools_free(&reg);
    db_close(db);
    rm_rf("/tmp/test_fork_src");
    rm_rf("/tmp/test_fork_ws");
    printf("  PASS test_extension_fork_roundtrip\n");
}

/* First-come name ownership: a promote may never change the owner of an
 * existing name; re-installing under the same owner still works. */
static void test_install_refuses_owner_takeover(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = open_seeded();
    char *err = NULL;
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    free(err); err = NULL;

    /* same owner: refresh succeeds */
    assert(extension_install(db, BUNDLE, "default", 0, &err) == 0);
    free(err); err = NULL;

    /* different owner: refused, existing row untouched */
    assert(extension_install(db, BUNDLE, "intruder", 0, &err) == -1);
    assert(err && strstr(err, "owned by 'default'"));
    free(err); err = NULL;
    char *owner = q1(db, "SELECT owner_agent FROM extensions WHERE name='nws'");
    assert(owner && strcmp(owner, "default") == 0); free(owner);

    db_close(db);
    printf("  PASS test_install_refuses_owner_takeover\n");
}

/* The bundled compaction (decision 5): shipped in the binary, installed with
 * no owner, published, and attached to every agent the first time — a fresh
 * DB, or one upgraded from before the bundle. After that, an agent's opt-out
 * (enabled=0, or the row gone) survives restarts; a new agent gets it from
 * agent_default_extensions; the operator's switches and the failure flag on
 * the hook survive a restart, and a new version starts the count over. */
static void test_install_builtin_compaction(void) {
    test_db_clean(TEST_DB);
    rm_rf("/tmp/extensions/compaction");
    sqlite3 *db = open_seeded();
    assert(config_registry_sync(db) == 0);
    assert(extension_install_builtin(db, TEST_DB) == 0);

    assert(qint(db, "SELECT owner_agent IS NULL AND published=1 AND enabled=1"
                    " FROM extensions WHERE name='compaction'") == 1);
    struct stat st;
    assert(stat("/tmp/extensions/compaction/compact.qjs", &st) == 0);
    assert(stat("/tmp/extensions/compaction/extension.json", &st) == 0);
    assert(qint(db, "SELECT COUNT(*) FROM hooks WHERE extension_name='compaction'"
                    " AND event='ContextThreshold' AND async=0 AND timeout=600"
                    " AND json_extract(input,'$.history')='all'"
                    " AND path='/tmp/extensions/compaction/compact.qjs'") == 1);
    assert(qint(db, "SELECT COUNT(*) FROM hooks WHERE extension_name='compaction'"
                    " AND event='CacheExpiring' AND async=0 AND timeout=600 AND pos=1"
                    " AND path='/tmp/extensions/compaction/compact.qjs'") == 1);
    /* Every agent there was has it, and runs it. */
    assert(qint(db, "SELECT COUNT(*) FROM agents a WHERE NOT EXISTS (SELECT 1 FROM"
                    " agent_extensions ae WHERE ae.agent_name=a.name"
                    " AND ae.extension_name='compaction' AND ae.enabled=1)") == 0);
    assert(hook_any(db, "default", HOOK_EVENT_CONTEXT_THRESHOLD) == 1);
    assert(hook_any(db, "default", HOOK_EVENT_CACHE_EXPIRING) == 1);

    /* Opt-outs survive a restart: switched off, and detached. */
    assert(sqlite3_exec(db,
        "UPDATE agent_extensions SET enabled=0 WHERE agent_name='grantee'"
        "  AND extension_name='compaction';"
        "DELETE FROM agent_extensions WHERE agent_name='intruder'"
        "  AND extension_name='compaction';", NULL, NULL, NULL) == SQLITE_OK);
    assert(extension_install_builtin(db, TEST_DB) == 0);
    assert(qint(db, "SELECT enabled FROM agent_extensions WHERE agent_name='grantee'"
                    " AND extension_name='compaction'") == 0);
    assert(qint(db, "SELECT COUNT(*) FROM agent_extensions WHERE agent_name='intruder'"
                    " AND extension_name='compaction'") == 0);
    assert(hook_any(db, "grantee", HOOK_EVENT_CONTEXT_THRESHOLD) == 0);

    /* A new agent gets it. */
    assert(sqlite3_exec(db,
        "INSERT INTO providers(name, base_url) VALUES('p','http://p');"
        "INSERT INTO models(id, provider_name, model) VALUES('p/m','p','m');"
        "INSERT INTO agent_models(agent_name, model_id, pos) VALUES('default','p/m',0);",
        NULL, NULL, NULL) == SQLITE_OK);
    char *err = NULL;
    assert(agent_definition_apply(db, "{\"name\":\"Newcomer\"}", "default",
                                  NULL, GRANT_ORIGIN("seed", 0), &err) == 0 ||
           (fprintf(stderr, "apply: %s\n", err ? err : "?"), 0));
    free(err);
    assert(qint(db, "SELECT enabled FROM agent_extensions WHERE agent_name='Newcomer'"
                    " AND extension_name='compaction'") == 1);

    /* Disabled by its failures, or switched off by the operator: a restart
     * keeps both. The extension switched off as a whole stays off too. */
    assert(sqlite3_exec(db, "UPDATE hooks SET disabled=1, fail_count=3, enabled=0"
                            " WHERE extension_name='compaction';"
                            "UPDATE extensions SET enabled=0 WHERE name='compaction';",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(extension_install_builtin(db, TEST_DB) == 0);
    assert(qint(db, "SELECT disabled=1 AND fail_count=3 AND enabled=0 FROM hooks"
                    " WHERE extension_name='compaction'") == 1);
    assert(qint(db, "SELECT enabled FROM extensions WHERE name='compaction'") == 0);
    /* A new version is new code: the count starts over; the switch stays. */
    assert(sqlite3_exec(db, "UPDATE extensions SET version='0.0.1' WHERE name='compaction'",
                        NULL, NULL, NULL) == SQLITE_OK);
    assert(extension_install_builtin(db, TEST_DB) == 0);
    assert(qint(db, "SELECT disabled=0 AND fail_count=0 AND enabled=0 FROM hooks"
                    " WHERE extension_name='compaction'") == 1);
    db_close(db);
    printf("  PASS test_install_builtin_compaction\n");
}

/* extension_install_builtin on a fresh scratch DB: telegram lands with no owner
 * published=1 with a manifest on disk; the channel row exists ('draft'); a
 * second run preserves an 'active' status. */
static void test_install_builtin(void) {
    test_db_clean(TEST_DB);
    rm_rf("/tmp/extensions/telegram");
    rm_rf("/tmp/extensions/cclaw-docs");
    sqlite3 *db = open_seeded();
    assert(db);
    /* Pre-existing agent: the docs install must backfill its attachment. */
    db_agent_upsert(db, "OldTimer", NULL, NULL);

    assert(extension_install_builtin(db, TEST_DB) == 0);

    /* cclaw-docs: skills-only, shipped (no owner), published, backfilled. */
    assert(qint(db, "SELECT owner_agent IS NULL FROM extensions"
                    " WHERE name='cclaw-docs'") == 1);
    assert(qint(db, "SELECT published FROM extensions WHERE name='cclaw-docs'") == 1);
    struct stat dst;
    assert(stat("/tmp/extensions/cclaw-docs/skills/configuring-cclaw/SKILL.md", &dst) == 0);
    assert(stat("/tmp/extensions/cclaw-docs/skills/cclaw-agents/SKILL.md", &dst) == 0);
    assert(stat("/tmp/extensions/cclaw-docs/skills/backup-restore/SKILL.md", &dst) == 0);
    assert(qint(db, "SELECT count(*) FROM agent_extensions"
                    " WHERE agent_name='OldTimer' AND extension_name='cclaw-docs'"
                    " AND enabled=1") == 1);
    /* An explicit opt-out (enabled=0) survives reinstall. */
    sqlite3_exec(db, "UPDATE agent_extensions SET enabled=0"
                     " WHERE agent_name='OldTimer' AND extension_name='cclaw-docs'",
                 NULL, NULL, NULL);
    assert(extension_install_builtin(db, TEST_DB) == 0);
    assert(qint(db, "SELECT enabled FROM agent_extensions"
                    " WHERE agent_name='OldTimer' AND extension_name='cclaw-docs'") == 0);
    assert(qint(db, "SELECT owner_agent IS NULL FROM extensions"
                    " WHERE name='telegram'") == 1);
    /* No owner means no owner-side rows either: no attachment or grants
     * minted for a phantom 'system' agent. */
    assert(qint(db, "SELECT count(*) FROM grants g JOIN tools t ON t.name=g.value"
                    " WHERE t.extension_name='telegram'") == 0);
    assert(qint(db, "SELECT published FROM extensions WHERE name='telegram'") == 1);
    struct stat st;
    assert(stat("/tmp/extensions/telegram/extension.json", &st) == 0);
    assert(stat("/tmp/extensions/telegram/channel.qjs", &st) == 0);
    /* Builtin bundles are born trust-active (specs/config.md); running is
     * gated separately by telegram.enabled. */
    char *status = q1(db, "SELECT status FROM channels WHERE name='telegram'");
    assert(status && strcmp(status, "active") == 0); free(status);

    /* Simulate an operator demotion, then rerun: status must survive. */
    assert(sqlite3_exec(db, "UPDATE channels SET status='draft' WHERE name='telegram'",
                         NULL, NULL, NULL) == SQLITE_OK);
    assert(extension_install_builtin(db, TEST_DB) == 0);
    status = q1(db, "SELECT status FROM channels WHERE name='telegram'");
    assert(status && strcmp(status, "draft") == 0); free(status);

    /* Operator-set routing must also survive reinstall: install owns only
     * the code-identity columns, never default_agent (open-door routing). */
    assert(sqlite3_exec(db, "UPDATE channels SET default_agent='OldTimer'"
                            " WHERE name='telegram'",
                         NULL, NULL, NULL) == SQLITE_OK);
    assert(extension_install_builtin(db, TEST_DB) == 0);
    char *route = q1(db, "SELECT default_agent FROM channels WHERE name='telegram'");
    assert(route && strcmp(route, "OldTimer") == 0); free(route);

    /* Launch gate (specs/config.md): trust AND enabled AND required keys. */
    unsetenv("CCLAW_TELEGRAM_ENABLED");
    unsetenv("CCLAW_TELEGRAM_BOT_TOKEN");
    char why[192];
    assert(channel_should_launch(db, "telegram", why, sizeof(why)) == 0);
    assert(strstr(why, "trust status draft"));
    sqlite3_exec(db, "UPDATE channels SET status='active' WHERE name='telegram'",
                 NULL, NULL, NULL);
    assert(channel_should_launch(db, "telegram", why, sizeof(why)) == 0);
    assert(strstr(why, "not enabled"));
    setenv("CCLAW_TELEGRAM_ENABLED", "1", 1);
    assert(channel_should_launch(db, "telegram", why, sizeof(why)) == 0);
    assert(strstr(why, "telegram.bot_token"));
    setenv("CCLAW_TELEGRAM_BOT_TOKEN", "123456:ABC", 1);
    assert(channel_should_launch(db, "telegram", why, sizeof(why)) == 1);
    unsetenv("CCLAW_TELEGRAM_ENABLED");
    unsetenv("CCLAW_TELEGRAM_BOT_TOKEN");

    /* A gate that could not be read is -1, never 0: the daemon stops a running
     * channel on a 0, so conflating "the query failed" with "the operator
     * turned this off" SIGTERMs a healthy channel over a DB blip. A genuinely
     * absent channel still answers 0 — the two must not collapse. */
    assert(channel_should_launch(db, "nosuchchannel", why, sizeof(why)) == 0);
    assert(strstr(why, "missing"));
    sqlite3 *schemaless = NULL;
    assert(sqlite3_open(":memory:", &schemaless) == SQLITE_OK);
    assert(channel_should_launch(schemaless, "telegram", why, sizeof(why)) == -1);
    assert(strstr(why, "lookup failed"));
    sqlite3_close(schemaless);

    /* The invariant has to hold for ALL four reads, not just the two that
     * prepare their own SQL. config_get() collapses "no such key" and "the
     * query failed" into the same NULL, so an unreadable config table used to
     * look exactly like `<ext>.enabled=0` — and channel_tick acts on that 0 by
     * SIGTERMing a healthy channel. Read 2 (<ext>.enabled): */
    sqlite3_set_authorizer(db, deny_config_reads, NULL);
    assert(channel_should_launch(db, "telegram", why, sizeof(why)) == -1);
    assert(strstr(why, "enabled lookup failed"));
    sqlite3_set_authorizer(db, NULL, NULL);

    /* Read 4 (each required key), isolated one read further in: enabled comes
     * from the env layer without touching the DB, the required-key list reads
     * only config.key/config.required, and only the value read is refused. */
    setenv("CCLAW_TELEGRAM_ENABLED", "1", 1);
    sqlite3_set_authorizer(db, deny_config_value, NULL);
    assert(channel_should_launch(db, "telegram", why, sizeof(why)) == -1);
    assert(strstr(why, "lookup failed") && strstr(why, "telegram.bot_token"));
    sqlite3_set_authorizer(db, NULL, NULL);
    unsetenv("CCLAW_TELEGRAM_ENABLED");

    db_close(db);
    rm_rf("/tmp/extensions/telegram");
    rm_rf("/tmp/extensions/cclaw-docs");
    printf("  PASS test_install_builtin\n");
}

/* agents[] component: validation, install-creates-agent, skip-existing,
 * caps refusal (specs/self-configuration.md). */
static void test_manifest_agents(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    write_file(BUNDLE "/scout.md", "You scout the weather.\n");
    write_file(BUNDLE "/extension.json",
        "{ \"name\": \"nws\","
        "  \"agents\": [ { \"name\": \"WxScout\", \"description\": \"weather agent\","
        "    \"sandbox_profile\": \"restricted\","
        "    \"system_prompt_file\": \"scout.md\","
        "    \"grants\": { \"tools\": [\"web_fetch\"] } } ] }");
    sqlite3 *db = open_seeded();
    assert(db);
    db_agent_upsert(db, "Owner", NULL, NULL);
    sqlite3_exec(db, "INSERT INTO grants(agent_name,kind,value,source)"
                     " VALUES('Owner','tool','web_fetch','operator')", NULL, NULL, NULL);
    /* Owner needs a routing list — manifest agents inherit it (R2). */
    sqlite3_exec(db,
        "INSERT OR IGNORE INTO providers(name,base_url) VALUES('p','http://p');"
        "INSERT OR IGNORE INTO models(id,provider_name,model)"
        " VALUES('p/m','p','m');"
        "INSERT INTO agent_models(agent_name,model_id,pos)"
        " VALUES('Owner','p/m',0);", NULL, NULL, NULL);

    /* Validation failures: bad name, bad profile, missing prompt file. */
    char *err = NULL;
    write_file(BUNDLE "/extension.json",
        "{\"name\":\"nws\",\"agents\":[{\"name\":\"lower\"}]}");
    assert(extension_manifest_validate(BUNDLE, &err) != 0);
    assert(err && strstr(err, "PascalCase")); free(err); err = NULL;
    write_file(BUNDLE "/extension.json",
        "{\"name\":\"nws\",\"agents\":[{\"name\":\"Ok\",\"sandbox_profile\":\"ultra\"}]}");
    assert(extension_manifest_validate(BUNDLE, &err) != 0);
    assert(err && strstr(err, "sandbox_profile")); free(err); err = NULL;
    write_file(BUNDLE "/extension.json",
        "{\"name\":\"nws\",\"agents\":[{\"name\":\"Ok\",\"system_prompt_file\":\"ghost.md\"}]}");
    assert(extension_manifest_validate(BUNDLE, &err) != 0);
    assert(err && strstr(err, "system_prompt_file")); free(err); err = NULL;

    /* Caps refusal: owner lacks shell_exec. */
    write_file(BUNDLE "/extension.json",
        "{ \"name\": \"nws\", \"agents\": [ { \"name\": \"WxScout\","
        "  \"grants\": { \"tools\": [\"shell_exec\"] } } ] }");
    assert(extension_install(db, BUNDLE, "Owner", 0, &err) != 0);
    assert(err && strstr(err, "exceeds creator")); free(err); err = NULL;

    /* Good install creates the agent with resolved prompt + capped grants. */
    write_file(BUNDLE "/extension.json",
        "{ \"name\": \"nws\","
        "  \"agents\": [ { \"name\": \"WxScout\", \"description\": \"weather agent\","
        "    \"sandbox_profile\": \"restricted\","
        "    \"system_prompt_file\": \"scout.md\","
        "    \"grants\": { \"tools\": [\"web_fetch\"] } } ] }");
    int rc = extension_install(db, BUNDLE, "Owner", 0, &err);
    if (rc != 0) fprintf(stderr, "install error: %s\n", err ? err : "?");
    assert(rc == 0);
    char *v = q1(db, "SELECT system_prompt FROM agents WHERE name='WxScout'");
    assert(v && strstr(v, "You scout the weather")); free(v);
    v = q1(db, "SELECT sandbox_profile FROM agents WHERE name='WxScout'");
    assert(v && strcmp(v, "restricted") == 0); free(v);
    assert(qint(db, "SELECT COUNT(*) FROM grants WHERE agent_name='WxScout'"
                    " AND kind='tool' AND value='web_fetch'") == 1);

    /* Re-install: existing agent skipped, not overwritten. */
    sqlite3_exec(db, "UPDATE agents SET description='operator edit'"
                     " WHERE name='WxScout'", NULL, NULL, NULL);
    assert(extension_install(db, BUNDLE, "Owner", 0, &err) == 0);
    v = q1(db, "SELECT description FROM agents WHERE name='WxScout'");
    assert(v && strcmp(v, "operator edit") == 0); free(v);
    assert(qint(db, "SELECT COUNT(*) FROM agents WHERE name='WxScout'") == 1);

    db_close(db);
    printf("  PASS test_manifest_agents\n");
}

/* extension_install seeds tool grants for the owner agent: one grant row
 * per $.tools[] entry (kind='tool', value=tool name). Re-install must be
 * idempotent (INSERT OR IGNORE — grant count unchanged). */
static void test_install_seeds_grants(void) {
    test_db_clean(TEST_DB);
    setup_bundle();
    sqlite3 *db = open_seeded();
    assert(db);

    char *err = NULL;
    int rc = extension_install(db, BUNDLE, "grantee", 0, &err);
    assert(rc == 0 && err == NULL);

    /* The fixture manifest declares one tool: "get_forecast". Verify that a
     * grants row was seeded for the owner 'grantee'. */
    assert(qint(db, "SELECT count(*) FROM grants WHERE agent_name='grantee'"
                    " AND kind='tool' AND value='get_forecast'") == 1);
    /* Verify grant attributes: expires_at must be NULL (permanent). */
    assert(qint(db, "SELECT count(*) FROM grants WHERE agent_name='grantee'"
                    " AND kind='tool' AND value='get_forecast'"
                    " AND expires_at IS NULL") == 1);

    /* Total tool grants for this owner = exactly the manifest tool count (1). */
    int grant_count = qint(db, "SELECT count(*) FROM grants WHERE agent_name='grantee'"
                               " AND kind='tool'");
    assert(grant_count == 1);

    /* Provenance: the promotion's own grant, tied to its approval. */
    assert(qint(db, "SELECT count(*) FROM grants WHERE agent_name='grantee'"
                    " AND kind='tool' AND source='extension'") == 1);

    /* Re-install: idempotent — grant count unchanged (INSERT OR IGNORE). */
    assert(extension_install(db, BUNDLE, "grantee", 0, &err) == 0);
    int grant_count2 = qint(db, "SELECT count(*) FROM grants WHERE agent_name='grantee'"
                                " AND kind='tool'");
    assert(grant_count2 == grant_count);

    /* A promote applied under approval #N records N. */
    sqlite3_exec(db, "DELETE FROM grants WHERE agent_name='grantee';"
                     "INSERT INTO approvals(id, session_id, tool_name, state)"
                     " VALUES(77, 1, 'extension_promote', 'approved');", NULL, NULL, NULL);
    assert(extension_install(db, BUNDLE, "grantee", 77, &err) == 0);
    assert(qint(db, "SELECT count(*) FROM grants WHERE agent_name='grantee'"
                    " AND source='extension' AND approval_id=77") == 1);

    db_close(db);
    printf("  PASS test_install_seeds_grants\n");
}

int main(void) {
    TEST_INIT();
    printf("test_extension_manifest:\n");
    test_install_ingests_rows();
    test_install_seeds_grants();
    test_reinstall_is_idempotent();
    test_tool_droppable();
    test_hook_ingest();
    test_config_lifecycle();
    test_validate_rejects_missing_handler();
    test_validate_rejects_bad_json();
    test_validate_rejects_bad_sections();
    test_extension_fork_roundtrip();
    test_install_refuses_owner_takeover();
    test_install_builtin();
    test_install_builtin_compaction();
    test_manifest_agents();
    /* cleanup */
    test_db_clean(TEST_DB);
    rm_rf("/tmp/test_ext_manifest_bundle");
    rm_rf("/tmp/extensions");
    printf("test_extension_manifest: ALL PASS\n");
    return 0;
}
