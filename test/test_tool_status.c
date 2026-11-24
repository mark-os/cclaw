/* Explicit tool status (D2) + signal attribution (D3).
 *
 * The point of all of this is that nothing reads the result TEXT to decide
 * whether a call failed. So the tests deliberately use failure messages that
 * do NOT start with "error:" — under the old strncmp sniff every one of them
 * would have been recorded as a success.
 *
 * No network: the shell tier runs local /bin/sh, the file tier runs
 * in-process, and the JS tier evaluates inline source. */
#define _GNU_SOURCE
#include "run_tool.h"
#include "tools.h"
#include "tool_js.h"
#include "secret_capture.h"
#include "db.h"
#include "secret.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_run_tool_shell.h"
#include "test_util.h"

static char WS[256];

/* ── the wire: a status byte, framed with the body it describes ────────── */

/* Success: a command that works frames a success status. */
static void test_frame_status_ok(void) {
    int status = -1;
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "echo ok";
    r.workspace = WS;
    r.sandbox = 0;      /* no namespace needed — this is a framing test */
    r.net_mode = 1;
    char *res = run_tool_shell_full(&r, NULL, &status);
    assert(status == 0);
    assert(res && strstr(res, "ok") != NULL);
    free(res);
}

/* Failure: the broker's own timeout. The status byte says "failed" while the
 * message text opens with "[timeout after ...]" — no "error:" anywhere. */
static void test_frame_status_error(void) {
    int status = -1;
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "sleep 5";
    r.timeout = 1;
    r.workspace = WS;
    r.sandbox = 0;
    r.net_mode = 1;
    char *res = run_tool_shell_full(&r, NULL, &status);
    assert(status == 1);
    assert(res && strncmp(res, "error:", 6) != 0);
    free(res);
}

/* The two halves of the regression the sniff could not catch: a failure is a
 * failure whatever it says, and prose that merely mentions "error:" is not
 * one. */
static void test_unprefixed_failure_is_flagged(void) {
    int is_error = 0;
    char *r = test_run_tier_status(RUNTOOL_TIER_JS, "js_eval",
                                   "{\"code\":\"throw new Error('nope')\"}",
                                   NULL, &is_error);
    assert(is_error == 1);
    free(r);

    /* And the converse: a successful call must NOT be flagged, even though its
     * output happens to contain the word "error". */
    is_error = 1;
    r = test_run_tier_status(RUNTOOL_TIER_JS, "js_eval",
                             "{\"code\":\"'error: this is just text'\"}",
                             NULL, &is_error);
    assert(is_error == 0);
    assert(r && strstr(r, "error: this is just text") != NULL);
    free(r);
}

/* ── secret capture keys off the status, not the prose ─────────────────── */

static void test_capture_skips_on_status(void) {
    const char *dbp = "/tmp/cclaw_test_tool_status.db";
    test_db_clean(dbp);
    unlink("/tmp/.cclaw_key");
    sqlite3 *db = test_db_open(dbp);
    assert(db);
    uint8_t key[32];
    assert(secret_key_load_or_create(dbp, key) == 0);
    db_set_secret_key(key);
    test_seed_agent(db, "TestAgent");

    /* Failure, phrased without the magic word: nothing may be captured. */
    char *out = secret_capture_apply(db, "TestAgent", "{\"save_secret\":\"S_KEY\"}",
                                     "HTTP 403 Forbidden", 1);
    assert(out && strstr(out, "skipped"));
    assert(!db_secret_exists(db, "S_KEY"));
    free(out);

    /* Success whose text merely looks like an error: capture proceeds. */
    out = secret_capture_apply(db, "TestAgent", "{\"save_secret\":\"T_KEY\"}",
                               "error: not really — this is the token", 0);
    assert(out && strstr(out, "saved"));
    assert(db_secret_exists(db, "T_KEY"));
    free(out);

    db_wipe_secret_key();
    sqlite3_close(db);
}

/* ── timeout parameter: honored, and capped ────────────────────────────── */

static void test_js_timeout_schema(void) {
    ToolRegistry reg;
    tools_init(&reg);
    JsEvalCtx jc = {0};
    jc.workspace = WS;
    assert(tool_js_eval_register(&reg, &jc) == 0);
    ToolEntry *e = tools_lookup(&reg, "js_eval");
    assert(e && e->parameters_json && strstr(e->parameters_json, "\"timeout\""));
    assert(e && e->parameters_json && strstr(e->parameters_json, "600"));
    tools_free(&reg);
}

/* The clamp itself: a raised timeout is honored up to the shared ceiling and
 * silently clamped past it. */
static void test_timeout_clamp(void) {
    assert(tool_timeout_clamp(0, 60) == 60);
    assert(tool_timeout_clamp(-5, 60) == 60);
    assert(tool_timeout_clamp(300, 60) == 300);
    assert(tool_timeout_clamp(99999, 60) == TOOL_TIMEOUT_MAX_SEC);
    assert(tool_timeout_clamp(TOOL_TIMEOUT_MAX_SEC, 60) == TOOL_TIMEOUT_MAX_SEC);
}

/* A raised timeout reaches the broker: a 3s sleep under a 1s budget times out,
 * the same command under a 5s budget does not. */
static void test_timeout_honored(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "sleep 3; echo survived";
    r.workspace = WS;
    r.sandbox = 0;
    r.net_mode = 1;

    r.timeout = 1;
    char *res = run_tool_shell(&r);
    assert(res && strstr(res, "timeout") != NULL);
    free(res);

    r.timeout = 8;
    res = run_tool_shell(&r);
    assert(res && strstr(res, "survived") != NULL);
    free(res);
}

/* ── D3: one cheap attribution for a signalled death ───────────────────── */

static void test_sigkill_attribution(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "kill -9 $$";
    r.workspace = WS;
    r.sandbox = 0;
    r.net_mode = 1;
    r.as_mb = 512;      /* limits configured → attribute to them */
    char *res = run_tool_shell(&r);
    assert(res && strstr(res, "SIGKILL") != NULL);
    assert(res && strstr(res, "resource limit") != NULL);
    free(res);
}

/* A clean exit gets no attribution line — the note must not become noise. */
static void test_no_attribution_on_clean_exit(void) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "exit 3";
    r.workspace = WS;
    r.sandbox = 0;
    r.net_mode = 1;
    char *res = run_tool_shell(&r);
    assert(res && strstr(res, "[exit 3]"));
    assert(res && strstr(res, "SIGKILL") == NULL);
    free(res);
}

int main(void) {
    TEST_INIT();
    printf("test_tool_status:\n");

    snprintf(WS, sizeof(WS), "/tmp/cclaw_tool_status_%d", (int)getpid());
    mkdir(WS, 0755);

    RUN(test_frame_status_ok);
    RUN(test_frame_status_error);
    RUN(test_unprefixed_failure_is_flagged);
    RUN(test_capture_skips_on_status);
    RUN(test_js_timeout_schema);
    RUN(test_timeout_clamp);
    RUN(test_timeout_honored);
    RUN(test_sigkill_attribution);
    RUN(test_no_attribution_on_clean_exit);

    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", WS);
    if (system(cmd) != 0) { /* best effort */ }

    return 0;
}
