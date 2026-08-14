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
#include "tool_web_fetch.h"
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

static int tests_run = 0, tests_failed = 0;
#define CHECK(name, cond) do { tests_run++; printf("  %s... ", name); \
    if (cond) printf("PASS\n"); else { tests_failed++; printf("FAIL\n"); } } while (0)

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
    CHECK("success frames status 0", status == 0);
    CHECK("success carries the body", res && strstr(res, "ok") != NULL);
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
    CHECK("timeout frames status 1", status == 1);
    CHECK("timeout text needs no 'error:' prefix",
          res && strncmp(res, "error:", 6) != 0);
    free(res);
}

/* The two halves of the regression the sniff could not catch: a failure is a
 * failure whatever it says, and prose that merely mentions "error:" is not
 * one. */
static void test_unprefixed_failure_is_flagged(void) {
    int is_error = 0;
    char *r = test_run_tier_status(RUNTOOL_TIER_JS, "js_eval",
                                   "{\"code\":\"throw new Error('nope')\"}",
                                   NULL, NULL, &is_error);
    CHECK("js exception sets the status", is_error == 1);
    free(r);

    /* And the converse: a successful call must NOT be flagged, even though its
     * output happens to contain the word "error". */
    is_error = 1;
    r = test_run_tier_status(RUNTOOL_TIER_JS, "js_eval",
                             "{\"code\":\"'error: this is just text'\"}",
                             NULL, NULL, &is_error);
    CHECK("prose mentioning 'error:' is not a failure", is_error == 0);
    CHECK("and the prose is returned verbatim",
          r && strstr(r, "error: this is just text") != NULL);
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

    /* Failure, phrased without the magic word: nothing may be captured. */
    char *out = secret_capture_apply(db, "{\"save_secret\":\"S_KEY\"}",
                                     "HTTP 403 Forbidden", 1);
    CHECK("capture skipped on failure status", out && strstr(out, "skipped"));
    CHECK("nothing stored on failure", !db_secret_exists(db, "S_KEY"));
    free(out);

    /* Success whose text merely looks like an error: capture proceeds. */
    out = secret_capture_apply(db, "{\"save_secret\":\"T_KEY\"}",
                               "error: not really — this is the token", 0);
    CHECK("capture proceeds on success status", out && strstr(out, "saved"));
    CHECK("value stored", db_secret_exists(db, "T_KEY"));
    free(out);

    db_wipe_secret_key();
    sqlite3_close(db);
}

/* ── timeout parameter: honored, and capped ────────────────────────────── */

static void test_timeout_schema(void) {
    ToolRegistry reg;
    tools_init(&reg);
    WebFetchCtx wc = {0};
    wc.workspace = WS;
    assert(tool_web_fetch_register(&reg, &wc) == 0);
    ToolEntry *e = tools_lookup(&reg, "web_fetch");
    CHECK("web_fetch declares timeout",
          e && e->parameters_json && strstr(e->parameters_json, "\"timeout\""));
    CHECK("web_fetch states the cap",
          e && e->parameters_json && strstr(e->parameters_json, "600"));
    tools_free(&reg);
}

static void test_js_timeout_schema(void) {
    ToolRegistry reg;
    tools_init(&reg);
    JsEvalCtx jc = {0};
    jc.workspace = WS;
    assert(tool_js_eval_register(&reg, &jc) == 0);
    ToolEntry *e = tools_lookup(&reg, "js_eval");
    CHECK("js_eval declares timeout",
          e && e->parameters_json && strstr(e->parameters_json, "\"timeout\""));
    CHECK("js_eval states the cap",
          e && e->parameters_json && strstr(e->parameters_json, "600"));
    tools_free(&reg);
}

/* The clamp itself: a raised timeout is honored up to the shared ceiling and
 * silently clamped past it. */
static void test_timeout_clamp(void) {
    CHECK("default when absent", tool_timeout_clamp(0, 60) == 60);
    CHECK("negative falls back", tool_timeout_clamp(-5, 60) == 60);
    CHECK("raise is honored", tool_timeout_clamp(300, 60) == 300);
    CHECK("cap is enforced",
          tool_timeout_clamp(99999, 60) == TOOL_TIMEOUT_MAX_SEC);
    CHECK("cap itself passes",
          tool_timeout_clamp(TOOL_TIMEOUT_MAX_SEC, 60) == TOOL_TIMEOUT_MAX_SEC);
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
    CHECK("short budget times out", res && strstr(res, "timeout") != NULL);
    free(res);

    r.timeout = 8;
    res = run_tool_shell(&r);
    CHECK("raised budget completes", res && strstr(res, "survived") != NULL);
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
    CHECK("SIGKILL is named", res && strstr(res, "SIGKILL") != NULL);
    CHECK("and attributed to a limit",
          res && strstr(res, "likely resource limit") != NULL);
    CHECK("with advice not to just retry",
          res && strstr(res, "don't just retry") != NULL);
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
    CHECK("nonzero exit is reported plainly", res && strstr(res, "[exit 3]"));
    CHECK("no signal attribution", res && strstr(res, "SIGKILL") == NULL);
    free(res);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    TEST_INIT();
    printf("test_tool_status:\n");

    snprintf(WS, sizeof(WS), "/tmp/cclaw_tool_status_%d", (int)getpid());
    mkdir(WS, 0755);

    test_frame_status_ok();
    test_frame_status_error();
    test_unprefixed_failure_is_flagged();
    test_capture_skips_on_status();
    test_timeout_schema();
    test_js_timeout_schema();
    test_timeout_clamp();
    test_timeout_honored();
    test_sigkill_attribution();
    test_no_attribution_on_clean_exit();

    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", WS);
    if (system(cmd) != 0) { /* best effort */ }

    printf("%d/%d checks passed\n", tests_run - tests_failed, tests_run);
    return tests_failed ? 1 : 0;
}
