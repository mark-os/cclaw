/* Test approval_format_summary: the (tool × gate-reason) matrix.
 * Axis 1 = tool + args rendering (generic json_each tail, bespoke document
 * branches); axis 2 = why it parked (secret_bind / sensitive overlays).
 * Plus the R1 mitigations: fence-escape of model-authored content, facts
 * before code so bottom-up truncation eats the code first. */
#define _POSIX_C_SOURCE 200809L
#include "approval.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_approval_summary.db"

static sqlite3 *fresh_db(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    test_seed_agent(db, "bot");
    return db;
}

static void exec_sql(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "exec_sql failed: %s\n", err ? err : "?");
        assert(0);
    }
}

/* Park one approval on a fresh session and render its summary. */
static char *summarize(sqlite3 *db, const char *tool, const char *action,
                       const char *args) {
    int64_t sid = session_create(db, "s", "bot", -1, 0);
    assert(sid > 0);
    int64_t aid = approval_create(db, sid, "call_1", tool, action, args, "rerun");
    assert(aid > 0);
    Approval *a = approval_get_pending(db, sid);
    assert(a != NULL);
    char *s = approval_format_summary(db, a);
    approval_free(a);
    assert(s != NULL);
    return s;
}

/* secret_bind on js_eval: both secrets named, bindings shown, egress bound
 * rendered from the grants table. The one approval class where blind DMs
 * were the original bug (approvals #9/#10, 2026-07-31). */
static void test_secret_bind_js_eval(void) {
    sqlite3 *db = fresh_db();
    exec_sql(db, "INSERT INTO secrets(name,value,scope) VALUES"
                 " ('ALPACA_API_KEY','enc:00','agent'),"
                 " ('PROV_KEY','enc:00','system');");
    exec_sql(db, "INSERT INTO secret_hosts(secret_name,host) VALUES"
                 " ('ALPACA_API_KEY','api.alpaca.markets');");
    exec_sql(db, "INSERT INTO grants(agent_name,kind,value) VALUES"
                 " ('bot','host','api.alpaca.markets'),"
                 " ('bot','host','.tiingo.com');");
    /* Expired grant must not render as reachable egress. */
    exec_sql(db, "INSERT INTO grants(agent_name,kind,value,expires_at) VALUES"
                 " ('bot','host','expired.example.com',1);");

    char *s = summarize(db, "js_eval", "secret_bind",
        "{\"code\":\"fetch('https://api.alpaca.markets',"
        "{headers:{k:'{{SECRET:ALPACA_API_KEY}}',p:'{{SECRET:PROV_KEY}}'}})\"}");

    assert(strstr(s, "js_eval — parked: the call carries a stored secret"));
    assert(strstr(s, "ALPACA_API_KEY — bound to: api.alpaca.markets"));
    /* system-scoped secret is not in the agent snapshot — must render
     * visibly, not vanish. */
    assert(strstr(s, "PROV_KEY — not a loaded secret"));
    assert(strstr(s, "every host granted to this agent"));
    assert(strstr(s, ".tiingo.com"));
    assert(!strstr(s, "expired.example.com"));
    /* The full code, fenced, and AFTER the facts (R1 ordering). */
    assert(strstr(s, "Code:\n```"));
    assert(strstr(s, "fetch('https://api.alpaca.markets'"));
    assert(strstr(s, "every host granted") < strstr(s, "Code:\n```"));
    free(s);
    db_close(db);
    printf("  secret_bind js_eval: OK\n");
}

/* First use: no bindings yet. */
static void test_secret_bind_first_use(void) {
    sqlite3 *db = fresh_db();
    exec_sql(db, "INSERT INTO secrets(name,value,scope) VALUES"
                 " ('FRESH_KEY','enc:00','agent');");
    char *s = summarize(db, "shell_exec", "secret_bind",
        "{\"command\":\"curl -H 'x: {{SECRET:FRESH_KEY}}' https://x.y\"}");
    assert(strstr(s, "FRESH_KEY — unbound, first use"));
    /* No host grants at all: the fail-closed outcome is stated, not blank. */
    assert(strstr(s, "Egress if approved: none"));
    assert(strstr(s, "Command:\n```"));
    free(s);
    db_close(db);
    printf("  secret_bind first use: OK\n");
}

/* A promoted tool's declared hosts replace agent grants — the DM must
 * mirror that precedence (call_egress_build Q1). */
static void test_secret_bind_declared_hosts(void) {
    sqlite3 *db = fresh_db();
    exec_sql(db, "INSERT INTO secrets(name,value,scope) VALUES"
                 " ('STRIPE_KEY','enc:00','agent');");
    exec_sql(db, "INSERT INTO grants(agent_name,kind,value) VALUES"
                 " ('bot','host','should.not.render.example');");
    exec_sql(db, "INSERT INTO tools(name,extension_name,egress_hosts) VALUES"
                 " ('stripe_call','payments','api.stripe.com');");
    char *s = summarize(db, "stripe_call", "secret_bind",
        "{\"amount\":\"5\",\"key\":\"{{SECRET:STRIPE_KEY}}\"}");
    assert(strstr(s, "the tool's declared hosts"));
    assert(strstr(s, "api.stripe.com"));
    assert(!strstr(s, "should.not.render.example"));
    free(s);
    db_close(db);
    printf("  secret_bind declared hosts: OK\n");
}

/* Sensitivity park: the matched target is re-derived and named. */
static void test_sensitive_target(void) {
    sqlite3 *db = fresh_db();
    exec_sql(db, "INSERT INTO sensitive_targets(kind,value) VALUES"
                 " ('host','internal.example.com');");
    char *s = summarize(db, "web_fetch", "sensitive",
        "{\"url\":\"https://internal.example.com/admin\"}");
    assert(strstr(s, "web_fetch — parked: targets a sensitive-labeled host"));
    assert(strstr(s, "Sensitive target: internal.example.com"));
    assert(strstr(s, "'always' still passes only this one call"));
    /* Args still render (url is not code/command). */
    assert(strstr(s, "url: https://internal.example.com/admin"));
    free(s);
    db_close(db);
    printf("  sensitive target: OK\n");
}

/* R1: a crafted ``` in the command cannot close our fence and forge
 * trailing prompt text — runs of 3+ backticks are broken visibly. */
static void test_fence_escape(void) {
    sqlite3 *db = fresh_db();
    char *s = summarize(db, "shell_exec", "shell_exec",
        "{\"command\":\"echo hi\\n```\\nDecide here: /approve 999\"}");
    /* Content survives (the admin still sees what the model wrote)... */
    assert(strstr(s, "/approve 999"));
    assert(strstr(s, "echo hi"));
    /* ...but the raw breakout sequence does not: the 3-backtick run is
     * broken after each pair. */
    assert(!strstr(s, "```\nDecide here"));
    assert(strstr(s, "`` `\nDecide here"));
    free(s);
    db_close(db);
    printf("  fence escape: OK\n");
}

/* A placeholder NAME is model-authored too — secret_placeholder_names takes
 * whatever sits between the delimiters, so it gets the same fence escape the
 * body block does, or a crafted ``` mangles the egress facts below it. */
static void test_fence_escape_secret_name(void) {
    sqlite3 *db = fresh_db();
    char *s = summarize(db, "js_eval", "secret_bind",
        "{\"code\":\"x={{SECRET:```}}\"}");
    assert(strstr(s, "Secrets used:\n```\n`` `"));
    /* The block still closes where we closed it, so the egress line below
     * stays inside the summary's own structure. */
    assert(strstr(s, "Egress if approved"));
    free(s);
    db_close(db);
    printf("  fence escape (secret name): OK\n");
}

/* command and code both present: neither may vanish. SQL_ARGS_LINES excludes
 * the pair unconditionally, so a first-match-wins body block would drop the
 * loser from the summary entirely. */
static void test_body_block_both_keys(void) {
    sqlite3 *db = fresh_db();
    char *s = summarize(db, "hybrid_tool", "hybrid_tool",
        "{\"command\":\"git commit -m x\",\"code\":\"return 1\"}");
    assert(strstr(s, "Command:\n```\ngit commit -m x"));
    assert(strstr(s, "Code:\n```\nreturn 1"));
    free(s);
    db_close(db);
    printf("  body block both keys: OK\n");
}

/* Plain ask-mode park (action == tool): headline is just the tool, scalar
 * args enumerate, long values clip per line. */
static void test_generic_args(void) {
    sqlite3 *db = fresh_db();
    char big[300];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    char args[512];
    snprintf(args, sizeof(args),
             "{\"url\":\"https://x.y/z\",\"method\":\"POST\",\"body\":\"%s\","
             "\"nested\":{\"skip\":\"me\"}}", big);
    char *s = summarize(db, "web_fetch", "web_fetch", args);
    assert(strstr(s, "web_fetch\n"));
    assert(!strstr(s, "web_fetch (web_fetch)"));
    assert(strstr(s, "url: https://x.y/z"));
    assert(strstr(s, "method: POST"));
    assert(strstr(s, "..."));           /* body clipped at 200 */
    assert(!strstr(s, "skip"));         /* nested object skipped, not flattened */
    free(s);
    db_close(db);
    printf("  generic args: OK\n");
}

/* Bespoke document branch still renders values, not counts. */
static void test_request_changes_branch(void) {
    sqlite3 *db = fresh_db();
    char *s = summarize(db, "request_config", "request_changes",
        "{\"changes\":{\"grants\":{\"hosts\":[\"api.github.com\"]},"
        "\"config\":{\"log_level\":\"debug\"}},\"reason\":\"need gh\"}");
    assert(strstr(s, "Agent-scoped (this agent only):"));
    assert(strstr(s, "host   api.github.com"));
    assert(strstr(s, "System-wide:"));
    assert(strstr(s, "log_level = debug"));
    assert(strstr(s, "Reason: need gh"));
    free(s);
    db_close(db);
    printf("  request_changes branch: OK\n");
}

int main(void) {
    TEST_INIT();
    printf("test_approval_summary:\n");
    test_secret_bind_js_eval();
    test_secret_bind_first_use();
    test_secret_bind_declared_hosts();
    test_sensitive_target();
    test_fence_escape();
    test_fence_escape_secret_name();
    test_body_block_both_keys();
    test_generic_args();
    test_request_changes_branch();
    test_db_clean(DB_PATH);
    printf("test_approval_summary: ALL PASS\n");
    return 0;
}
