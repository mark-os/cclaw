/* test_tool_hosts.c — Q1 (network reach by tool kind) and the copy-on-promote
 * half of Q4.
 *
 * A promoted tool whose manifest declares hosts reaches exactly those hosts:
 * the declaration REPLACES the agent's host grants for calls of that tool. A
 * tool that declares none — every builtin, every draft — runs under the
 * agent's grants, unchanged. Drafts are inert because they are never ingested,
 * so the whole "promotion activates the declaration" rule is one DB lookup
 * (db_tool_declared_hosts) plus the manifest ingest that fills it.
 *
 * Every denial assertion below carries a positive control in the same
 * function: an empty host list is indistinguishable from a broken query. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "extension_manifest.h"
#include "host_match.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HOME_DIR "/tmp/cclaw_test_tool_hosts_home"
#define TEST_DB  HOME_DIR "/cclaw.db"
#define BUNDLE   "/tmp/cclaw_test_tool_hosts_bundle"

static sqlite3 *g_db;

static void exec_ok(const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "sql failed: %s\n", err ? err : "?");
    assert(rc == SQLITE_OK);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static void hosts_free(char **h, int n) {
    for (int i = 0; i < n; i++) free(h[i]);
    free(h);
}

/* ── Q1: which hosts a call gets ──────────────────────────────────── */

static void test_declared_replaces_grants(void) {
    /* The agent holds a host grant for evil.example — the ambient reach that
     * arms shell/web/js. A promoted tool declaring api.weather.test must
     * reach the declared host and NOT the granted one. */
    exec_ok("INSERT INTO grants(agent_name, kind, value)"
            " VALUES('Alice','host','evil.example');"
            "INSERT INTO extensions(name, path, owner_agent, published)"
            " VALUES('weather','" HOME_DIR "/extensions/weather','Alice',1);"
            "INSERT INTO tools(name, extension_name, path, egress_hosts)"
            " VALUES('get_forecast','weather',"
            "        '" HOME_DIR "/extensions/weather/f.qjs',"
            "        'api.weather.test,.weather.test');");

    int n = 0;
    char **h = db_tool_declared_hosts(g_db, "get_forecast", &n);
    assert(h && n == 2);                                  /* positive control */
    assert(host_covered(h, (size_t)n, "api.weather.test"));
    assert(host_covered(h, (size_t)n, "eu.weather.test"));
    /* Replacement, not union: the agent's grant does not widen the tool. */
    assert(!host_covered(h, (size_t)n, "evil.example"));
    hosts_free(h, n);
    printf("  PASS test_declared_replaces_grants\n");
}

static void test_undeclared_tool_uses_agent_grants(void) {
    /* A promoted tool with no declaration, and a builtin: both must answer
     * "no declared hosts", which is what makes the caller fall back to the
     * agent's grants. get_forecast above is the positive control. */
    exec_ok("INSERT INTO tools(name, extension_name, path)"
            " VALUES('get_alerts','weather','" HOME_DIR "/extensions/weather/a.qjs');"
            /* A builtin row is written with extension_name NULL. Hand-set
             * egress_hosts on one anyway: it must stay inert, or an agent
             * with db write reach could self-grant egress. */
            "INSERT INTO tools(name, extension_name, path, egress_hosts)"
            " VALUES('shell_exec', NULL, NULL, 'attacker.example');");

    int n = 1;
    char **h = db_tool_declared_hosts(g_db, "get_alerts", &n);
    assert(h == NULL && n == 0);
    h = db_tool_declared_hosts(g_db, "shell_exec", &n);
    assert(h == NULL && n == 0);
    /* Positive control: the same call on the declaring tool still answers. */
    h = db_tool_declared_hosts(g_db, "get_forecast", &n);
    assert(h && n == 2);
    hosts_free(h, n);
    printf("  PASS test_undeclared_tool_uses_agent_grants\n");
}

static void test_draft_declaration_is_inert(void) {
    /* A draft's manifest may declare whatever it likes — nothing ingested it,
     * so there is no tools row and no declared reach. An agent-authored
     * declaration honored pre-approval would be self-granted egress. */
    mkdir(BUNDLE, 0755);
    write_file(BUNDLE "/extension.json",
        "{\"name\":\"drafty\",\"version\":\"0.1.0\",\"tools\":[{"
        "\"name\":\"draft_tool\",\"description\":\"d\","
        "\"parameters\":{\"type\":\"object\"},\"handler\":\"h.qjs\","
        "\"hosts\":[\"api.draft.test\"]}]}");
    write_file(BUNDLE "/h.qjs", "'ok'");
    /* The draft validates — it is a promotable bundle, not a broken one. */
    char *err = NULL;
    assert(extension_manifest_validate(BUNDLE, &err) == 0);
    free(err);

    int n = 1;
    char **h = db_tool_declared_hosts(g_db, "draft_tool", &n);
    assert(h == NULL && n == 0);

    /* Positive control: promote the very same bundle and the same lookup now
     * answers — the declaration activates at promotion, and only there. */
    err = NULL;
    assert(extension_install(g_db, BUNDLE, "Alice", &err) == 0);
    free(err);
    h = db_tool_declared_hosts(g_db, "draft_tool", &n);
    assert(h && n == 1 && strcmp(h[0], "api.draft.test") == 0);
    hosts_free(h, n);
    printf("  PASS test_draft_declaration_is_inert\n");
}

static void test_manifest_refuses_unpinned_hosts(void) {
    mkdir(BUNDLE "2", 0755);
    write_file(BUNDLE "2/h.qjs", "'ok'");
    write_file(BUNDLE "2/extension.json",
        "{\"name\":\"wildy\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"hosts\":[\"*\"]}]}");
    char *err = NULL;
    assert(extension_manifest_validate(BUNDLE "2", &err) != 0);
    assert(err && strstr(err, "unpin"));
    free(err);

    /* A URL instead of a host would silently never match host_match's rules. */
    write_file(BUNDLE "2/extension.json",
        "{\"name\":\"wildy\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"hosts\":[\"https://api.example.com/v1\"]}]}");
    err = NULL;
    assert(extension_manifest_validate(BUNDLE "2", &err) != 0);
    free(err);

    /* hosts must be a list, not a bare string. */
    write_file(BUNDLE "2/extension.json",
        "{\"name\":\"wildy\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"hosts\":\"api.example.com\"}]}");
    err = NULL;
    assert(extension_manifest_validate(BUNDLE "2", &err) != 0);
    free(err);

    /* Positive control: a real host list validates. */
    write_file(BUNDLE "2/extension.json",
        "{\"name\":\"wildy\",\"tools\":[{\"name\":\"t\",\"handler\":\"h.qjs\","
        "\"hosts\":[\"api.example.com\",\".example.com\"]}]}");
    err = NULL;
    assert(extension_manifest_validate(BUNDLE "2", &err) == 0);
    free(err);
    printf("  PASS test_manifest_refuses_unpinned_hosts\n");
}

/* ── Q4: copy-on-promote ──────────────────────────────────────────── */

static void test_store_path_boundary(void) {
    /* extensions.path may only point into the shared store. A workspace path
     * is the case that matters: the draft stays the agent's editable copy, so
     * a promoted tool loading from it would let the agent rewrite approved
     * code after the approval. */
    assert(!extension_path_in_store(g_db,
        HOME_DIR "/agents/Alice/workspace/extensions/weather/f.qjs"));
    assert(!extension_path_in_store(g_db, "/tmp/elsewhere/f.qjs"));
    assert(!extension_path_in_store(g_db, HOME_DIR "/extensions"));
    assert(!extension_path_in_store(g_db, NULL));
    /* Positive control: the real store path is accepted. */
    assert(extension_path_in_store(g_db,
        HOME_DIR "/extensions/weather/f.qjs"));
    printf("  PASS test_store_path_boundary\n");
}

static void test_install_writes_store_paths(void) {
    /* The bundle lives in a workspace; after promotion every registered path
     * points at the store copy, never back at the editable draft. */
    char *p = NULL;
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT e.path || '|' || t.path FROM extensions e JOIN tools t"
        " ON t.extension_name=e.name WHERE e.name='drafty'",
        -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    p = test_strdup_((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    assert(p && strstr(p, HOME_DIR "/extensions/drafty") == p);
    assert(!strstr(p, BUNDLE "/"));
    free(p);
    printf("  PASS test_install_writes_store_paths\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_tool_hosts:\n");
    test_db_clean(TEST_DB);
    mkdir(HOME_DIR, 0755);
    g_db = test_db_open(TEST_DB);
    assert(g_db);
    test_seed_agent(g_db, "Alice");

    test_declared_replaces_grants();
    test_undeclared_tool_uses_agent_grants();
    test_draft_declaration_is_inert();
    test_manifest_refuses_unpinned_hosts();
    test_store_path_boundary();
    test_install_writes_store_paths();

    db_close(g_db);
    test_db_clean(TEST_DB);
    printf("All tool host declaration tests passed.\n");
    return 0;
}
