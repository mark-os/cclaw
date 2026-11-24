/* Dispatch-gate credential rule (runtime-bind-discovery, D17): an unbound or
 * uncovered secret DENIES inline — no park, no approval row, the error names
 * the secret and (where static) the host, and points at request_config
 * secret_bindings. Positive control: a bound+covered call passes the gate.
 * Sensitivity axis is orthogonal and still parks.
 *
 * Drives the real dispatch_tool() path (proc-globals + AgentSetup); the
 * positive control forks the real --run-tool broker via /proc/self/exe, so
 * main() guards that argv like test_channel_deaf does for --channel. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "agent_setup.h"
#include "config.h"
#include "db.h"
#include "dispatch.h"
#include "extension_manifest.h"
#include "proc.h"
#include "run_tool.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_secret_gate.db"

static sqlite3 *g_db;
static int64_t g_sid;
static int g_call_seq;

static void exec_sql(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "exec_sql failed: %s\n", err ? err : "?");
        assert(0);
    }
}

/* Seed one claimed tool call the way the dispatch loop would see it and run
 * it through dispatch_tool. Returns the dispatch rc; *out_result (optional)
 * gets the tool-result content if one was written for this call. */
static int run_call_as(const char *agent, int64_t sid, const char *tool,
                       const char *args, char **out_result);
static int run_call(const char *tool, const char *args, char **out_result) {
    return run_call_as("bot", g_sid, tool, args, out_result);
}

static int run_call_as(const char *agent, int64_t sid, const char *tool,
                       const char *args, char **out_result) {
    int64_t g_sid_saved = g_sid;
    g_sid = sid;
    int rc;
    char call_id[32];
    snprintf(call_id, sizeof(call_id), "call_%d", ++g_call_seq);

    Message as = {.role = ROLE_ASSISTANT, .content = ""};
    int64_t eid = entry_append_with_iteration(g_db, g_sid, &as, 1);
    assert(eid > 0);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO tool_calls(session_id,entry_id,call_id,name,status)"
             " VALUES(%lld,%lld,'%s','%s','running');",
             (long long)g_sid, (long long)eid, call_id, tool);
    exec_sql(sql);

    PendingToolCall tc = {
        .call_id = call_id, .name = (char *)tool, .arguments = (char *)args,
        .entry_id = eid, .iteration_id = 1,
    };
    rc = dispatch_tool(g_sid, agent, &tc);

    if (out_result) {
        /* Inline errors append a role-3 entry but don't stamp
         * result_entry_id (that's the async-completion path) — read the
         * newest tool entry after this call's assistant entry. */
        *out_result = NULL;
        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(g_db,
            "SELECT content FROM entries WHERE session_id=?1 AND role=3"
            " AND id>?2 ORDER BY id DESC LIMIT 1", -1, &s, NULL)
            == SQLITE_OK);
        sqlite3_bind_int64(s, 1, g_sid);
        sqlite3_bind_int64(s, 2, eid);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(s, 0);
            if (v) *out_result = strdup(v);
        }
        sqlite3_finalize(s);
    }
    g_sid = g_sid_saved;
    return rc;
}

#define approvals_count() test_qi(g_db, "SELECT COUNT(*) FROM approvals")

/* Using a secret is a grant (v60): before the agent holds a 'secret' grant,
 * naming the secret is refused inline — before any binding check, before
 * anything can park — and the refusal names the secret, the agent and the
 * way to ask. Covers both reference forms: {{SECRET:X}} in web_fetch's args and
 * $CCLAW_SECRET_X on the shell. Both secrets here are ENVIRONMENT
 * secrets (CCLAW_SECRET_*), which have no secrets row: the rule is by name. */
static void test_ungranted_secret_refused(void) {
    char *r = NULL;
    int rc = run_call("web_fetch",
        "{\"url\":\"https://api.x.test/e\","
        "\"headers\":\"k: {{SECRET:ALPACA_KEY}}\"}", &r);
    assert(rc == 1);
    assert(r && strstr(r, "secret ALPACA_KEY is not granted to agent bot"));
    assert(strstr(r, "grants"));
    assert(strstr(r, "request_config"));
    free(r);
    r = NULL;
    rc = run_call("shell_exec", "{\"command\":\"echo $CCLAW_SECRET_GH_TOKEN\"}", &r);
    assert(rc == 1);
    assert(r && strstr(r, "secret GH_TOKEN is not granted to agent bot"));
    free(r);
    /* An unknown name is still the typo error, not a grant refusal. */
    r = NULL;
    rc = run_call("web_fetch",
        "{\"url\":\"https://api.x.test/e\",\"headers\":\"k: {{SECRET:NOPE}}\"}", &r);
    assert(rc == 1 && r && strstr(r, "names no known secret"));
    free(r);
    assert(approvals_count() == 0);
}

/* Unbound secret, static url: inline error names secret + url host and the
 * request_config route; nothing parks. */
static void test_unbound_denies_inline(void) {
    char *r = NULL;
    int rc = run_call("web_fetch",
        "{\"url\":\"https://api.x.test/e\","
        "\"headers\":\"k: {{SECRET:ALPACA_KEY}}\"}", &r);
    assert(rc == 1);                       /* handled inline, not parked */
    assert(r != NULL);
    assert(strstr(r, "ALPACA_KEY has no host binding"));
    assert(strstr(r, "secret_bindings"));
    assert(strstr(r, "api.x.test"));       /* the url argument names the host */
    assert(approvals_count() == 0);        /* the park class is gone */
    free(r);
}

/* Bound elsewhere, url host uncovered: the error names the mismatch pair. */
static void test_uncovered_host_denies(void) {
    assert(db_secret_host_bind(g_db, "ALPACA_KEY", "api.other.test") == 0);
    char *r = NULL;
    int rc = run_call("web_fetch",
        "{\"url\":\"https://api.x.test/e\","
        "\"headers\":\"k: {{SECRET:ALPACA_KEY}}\"}", &r);
    assert(rc == 1);
    assert(r && strstr(r, "ALPACA_KEY is not bound to api.x.test"));
    assert(approvals_count() == 0);
    free(r);
}

/* Positive control: bound + covered passes the gate — the call is dispatched
 * (forked broker), not answered with a binding error. Whatever the child
 * then returns (proxy refusal, sandbox refusal on hosts without userns) is
 * downstream of the gate — except that it must have run: web_fetch's url
 * travels inside the JSON `args` its script is evaluated with, and a secret
 * holding a quote and a backslash, interpolated unescaped, ended that JSON
 * string early (a SyntaxError, or script written by the secret's value). */
static void test_bound_covered_passes(void) {
    assert(db_secret_host_bind(g_db, "ALPACA_KEY", "127.0.0.1") == 0);
    assert(db_secret_host_bind(g_db, "QUOTED", "127.0.0.1") == 0);
    char *r = NULL;
    int rc = run_call("web_fetch",
        "{\"url\":\"http://127.0.0.1:1/x?q={{SECRET:QUOTED}}\","
        "\"headers\":\"k: {{SECRET:ALPACA_KEY}}\"}", &r);
    assert(rc != 1 && rc != 2);            /* dispatched, not denied/parked */
    assert(r == NULL);                     /* no result yet — child owns it */
    assert(approvals_count() == 0);
    /* Reap so the forked child can't outlive the test. */
    for (int i = 0; i < 100; i++) {
        reap_children();
        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(g_db,
            "SELECT status FROM tool_calls WHERE session_id=?1"
            " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_int64(s, 1, g_sid);
        assert(sqlite3_step(s) == SQLITE_ROW);
        int done = strcmp((const char *)sqlite3_column_text(s, 0), "done") == 0;
        sqlite3_finalize(s);
        if (done) break;
        usleep(100 * 1000);
    }
    char q[160];
    snprintf(q, sizeof(q), "SELECT content FROM entries WHERE session_id=%lld"
             " AND role=3 ORDER BY id DESC LIMIT 1", (long long)g_sid);
    char *out = test_qs(g_db, q);
    assert(out && !strstr(out, "SyntaxError"));
    free(out);
}

/* Runtime discovery (shell, no static target): the call runs narrowed,
 * the proxy denies the unbound host, and the deny summary carries the
 * parent-composed note naming the secret and its bindings — so the model
 * learns the deny was a missing binding, not a missing host grant. */
static void test_runtime_deny_note(void) {
    assert(db_secret_host_bind(g_db, "GH_TOKEN", "api.other.test") == 0);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT COALESCE(MAX(id),0) FROM entries", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int64_t before = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    /* An https CONNECT routes through the proxy and is denied by name (an
     * in-namespace loopback connect, or a plain-http GET via the proxy env,
     * never reaches the policy path). */
    int rc = run_call("shell_exec",
        "{\"command\":\"curl -s --max-time 5 -H 'x: {{SECRET:GH_TOKEN}}'"
        " https://denied.test/\"}", NULL);
    assert(rc != 1 && rc != 2);            /* dispatched */
    char *r = NULL;
    for (int i = 0; i < 100 && !r; i++) {
        reap_children();
        assert(sqlite3_prepare_v2(g_db,
            "SELECT content FROM entries WHERE session_id=?1 AND role=3"
            " AND id>?2 ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_int64(s, 1, g_sid);
        sqlite3_bind_int64(s, 2, before);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(s, 0);
            if (v) r = strdup(v);
        }
        sqlite3_finalize(s);
        if (!r) usleep(100 * 1000);
    }
    assert(r != NULL);
    if (!strstr(r, "proxy blocked")) {
        /* No functioning namespace/proxy on this host — the broker refused
         * fail-closed before any egress decision; nothing to assert. */
        printf("  SKIP test_runtime_deny_note (no proxy deny: %.60s...)\n", r);
        free(r);
        return;
    }
    assert(strstr(r, "GH_TOKEN -> api.other.test"));
    assert(strstr(r, "secret_bindings"));
    assert(!strstr(r, "grants.hosts"));
    free(r);
}

/* Sensitivity is the orthogonal axis: a bound, covered call to a
 * sensitive-labeled host still parks per-call (park_reason='sensitive_target') — and no
 * approvals row with park_reason='secret_bind' is creatable anywhere. */
static void test_sensitivity_still_parks(void) {
    exec_sql("INSERT INTO sensitive_targets(kind,value)"
             " VALUES('host','bank.test');");
    assert(db_secret_host_bind(g_db, "ALPACA_KEY", "bank.test") == 0);
    char *r = NULL;
    int rc = run_call("web_fetch",
        "{\"url\":\"https://bank.test/x\","
        "\"headers\":\"k: {{SECRET:ALPACA_KEY}}\"}", &r);
    assert(rc == 2);                       /* parked */
    free(r);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT COUNT(*), SUM(park_reason='sensitive_target'), SUM(park_reason='secret_bind')"
        " FROM approvals", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    assert(sqlite3_column_int(s, 1) == 1);
    assert(sqlite3_column_int(s, 2) == 0);
    sqlite3_finalize(s);
}

/* The shared tool setup is initialised for one agent (the daemon's default),
 * but request_config answers about the agent whose turn it is — what it
 * already holds decides what is left to ask for. Before this was bound per
 * call, a second agent asking for a secret the first agent held was told
 * "already in effect" and nothing parked (seen on a throwaway daemon). */
static void test_request_config_answers_for_the_caller(void) {
    db_agent_upsert(g_db, "Eve", NULL, NULL);
    exec_sql("INSERT INTO grants(agent_name,kind,value,source)"
             " VALUES('Eve','tool','request_config','operator');");
    int64_t eve_sid = session_create(g_db, "e", "Eve", -1, 0);
    char *r = NULL;
    int rc = run_call_as("Eve", eve_sid, "request_config",
        "{\"changes\":{\"grants\":{\"secrets\":[\"ALPACA_KEY\"]}}}", &r);
    if (r) fprintf(stderr, "  request_config answered: %s\n", r);
    assert(rc == 2 && r == NULL);              /* parked for Eve */
    assert(db_scalar_i64(g_db, "SELECT COUNT(*) FROM approvals WHERE session_id=?1"
                         " AND tool_name='request_config' AND state='pending'",
                         eve_sid, -1) == 1);
}

int main(int argc, char *argv[]) {
    /* The positive control re-execs this binary as the --run-tool broker. */
    if (argc >= 2 && strcmp(argv[1], "--run-tool") == 0)
        return run_tool_main();
    (void)argv;
    TEST_INIT();

    test_db_clean(DB_PATH);
    char ws[128];
    snprintf(ws, sizeof(ws), "/tmp/cclaw_test_secret_gate_ws_%d", getpid());
    mkdir(ws, 0755);
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    setenv("CCLAW_WORKSPACE", ws, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    /* Loaded before agent_setup_init: shell_secrets_collect eats the env. */
    setenv("CCLAW_SECRET_ALPACA_KEY", "topsecret-value", 1);
    setenv("CCLAW_SECRET_GH_TOKEN", "another-secret", 1);
    setenv("CCLAW_SECRET_QUOTED", "a\"b\\c", 1);

    g_db = test_db_open(DB_PATH);
    assert(g_db);
    db_agent_upsert(g_db, "bot", NULL, NULL);
    exec_sql("INSERT INTO grants(agent_name,kind,value,source)"
             " VALUES('bot','tool','web_fetch','operator'),"
             "       ('bot','tool','shell_exec','operator');");
    g_sid = session_create(g_db, "t", "bot", -1, 0);
    assert(g_sid > 0);
    /* web_fetch is the shipped web-fetch extension, attached on install. */
    assert(extension_install_builtin(g_db, DB_PATH) == 0);

    Config *cfg = config_load(g_db);
    assert(cfg);
    proc_set_db(g_db);
    proc_set_cfg(cfg);
    static AgentSetup setup;
    assert(agent_setup_init(&setup, g_db, g_sid, cfg, "bot") == 0);
    proc_set_tool_setup(&setup);

    RUN(test_ungranted_secret_refused);
    /* From here on the agent may use both: the binding rule is what's left. */
    assert(agent_config_grant(g_db, "bot", "secret", "ALPACA_KEY", 0,
                              GRANT_ORIGIN("operator", 0)) == 0);
    assert(agent_config_grant(g_db, "bot", "secret", "GH_TOKEN", 0,
                              GRANT_ORIGIN("operator", 0)) == 0);
    assert(agent_config_grant(g_db, "bot", "secret", "QUOTED", 0,
                              GRANT_ORIGIN("operator", 0)) == 0);
    RUN(test_unbound_denies_inline);
    RUN(test_uncovered_host_denies);
    RUN(test_bound_covered_passes);
    RUN(test_runtime_deny_note);
    RUN(test_sensitivity_still_parks);
    RUN(test_request_config_answers_for_the_caller);

    agent_setup_destroy(&setup);
    config_free(cfg);
    db_close(g_db);
    test_db_clean(DB_PATH);
    char cmd[160];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    (void)!system(cmd);
    return 0;
}
