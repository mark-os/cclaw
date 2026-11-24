/* Regression: a parked approval must not orphan its tool call across a CLI exit.
 *
 * The bug (plan/TODO.md, 2026-07-31): in `-p` mode the non-tty auto-deny fired
 * from inside handle_approval_park, while the tool call was still claimed
 * 'running'. approval_is_post_window() tests for 'pending', so it read the
 * decision as *late* — inbox notice only, no tool result — and the dispatch
 * loop then unclaimed the call back to 'pending'. The CLI's `done:` path forced
 * the session idle before db_recover_stale_sessions ran, so recovery never saw
 * the orphan either. The next run re-dispatched the STALE call first (session-
 * wide ORDER BY rowid) and parked a fresh approval carrying the OLD document,
 * while the live call never ran.
 *
 * Two `-p` runs against a mock LLM reproduce the whole chain:
 *   run 1: model calls request_config (call_A, "doc A") → parks → left pending
 *          (2026-11-22: a no-approver run no longer denies; the call is
 *          answered "waiting for the operator" and the request stays open)
 *   run 2: model calls request_config (call_B, "doc B")
 * Post-fix, call_A is answered in run 1 and call_B parks its own document.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "mock_server.h"
#include "test_integ.h"
#include <limits.h>
#include <sys/stat.h>

#define DB_PATH "/tmp/cclaw_integ_approval_park.db"
#define LOG1    "/tmp/cclaw_integ_approval_park_run1.log"
#define LOG2    "/tmp/cclaw_integ_approval_park_run2.log"
/* Real directories: a grant path that doesn't exist canonicalizes to its
 * nearest existing ancestor (/tmp), which holds the test DB — and a grant
 * covering cclaw.db is refused before it can ever park. */
#define DIR_A   "/tmp/cclaw_park_a"
#define DIR_B   "/tmp/cclaw_park_b"
static int g_exit = -1;   /* exit status of the last -p run */
static IntegEnv g_env = {.db = DB_PATH};
#define db_count(sql) integ_db_int(DB_PATH, (sql), NULL)
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

/* Turn 1: ask for a read grant on document A. */
static const char *RESP_CALL_A =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_A\",\"type\":\"function\",\"function\":"
    "{\"name\":\"request_config\",\"arguments\":"
    "\"{\\\"action\\\":\\\"request_changes\\\","
    "\\\"changes\\\":{\\\"grants\\\":{\\\"read_paths\\\":[\\\"" DIR_A "\\\"]}},"
    "\\\"reason\\\":\\\"doc A\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

/* Turn 2: a different document — must park under its own tool_call_id. */
static const char *RESP_CALL_B =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_B\",\"type\":\"function\",\"function\":"
    "{\"name\":\"request_config\",\"arguments\":"
    "\"{\\\"action\\\":\\\"request_changes\\\","
    "\\\"changes\\\":{\\\"grants\\\":{\\\"read_paths\\\":[\\\"" DIR_B "\\\"]}},"
    "\\\"reason\\\":\\\"doc B\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static const char *RESP_FINAL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"acknowledged\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

/* ── helpers ─────────────────────────────────────────────────────── */

/* Exit status of a -p run that left an approval pending. */
#define EXIT_LEFT_PENDING 3

/* ── the two assertions blocks ───────────────────────────────────── */

/* After run 1: the parked call must have been *answered* — a tool_result entry
 * plus status 'done'. Pre-fix it is left 'pending' with no result. */
static int check_run1(void) {
    int64_t n;

    n = db_count("SELECT COUNT(*) FROM tool_calls WHERE call_id='call_A'"
                 " AND status='done';");
    if (n != 1) {
        fprintf(stderr, "FAIL run1: call_A not marked done (matching rows=%lld)\n",
                (long long)n);
        return 1;
    }

    n = db_count("SELECT COUNT(*) FROM entries WHERE role=3"
                 " AND tool_call_id='call_A';");
    if (n != 1) {
        fprintf(stderr, "FAIL run1: call_A has no tool_result entry (rows=%lld)\n",
                (long long)n);
        return 1;
    }

    /* Nobody was there to decide, so nobody denied: the call is answered
     * "waiting for the operator", told not to ask again (what stops the
     * re-request loop without a false denial), and the request stays open
     * for `cclaw approvals`. */
    n = db_count("SELECT COUNT(*) FROM entries WHERE role=3"
                 " AND tool_call_id='call_A'"
                 " AND content LIKE '%waiting for the operator%'"
                 " AND content LIKE '%Do NOT request it again%';");
    if (n != 1) {
        fprintf(stderr, "FAIL run1: call_A result does not read as left pending\n");
        return 1;
    }
    n = db_count("SELECT COUNT(*) FROM approvals WHERE tool_call_id='call_A'"
                 " AND state='pending';");
    if (n != 1) {
        fprintf(stderr, "FAIL run1: call_A's approval was decided by nobody (pending rows=%lld)\n",
                (long long)n);
        return 1;
    }
    n = db_count("SELECT COUNT(*) FROM sessions WHERE state!='idle';");
    if (n != 0) {
        fprintf(stderr, "FAIL run1: session not idle after the run\n");
        return 1;
    }
    if (g_exit != EXIT_LEFT_PENDING) {
        fprintf(stderr, "FAIL run1: exit status %d, want %d (approval left pending)\n",
                g_exit, EXIT_LEFT_PENDING);
        return 1;
    }
    return 0;
}

/* After run 2: the live call parks its own document, and the run-1 call is
 * never re-dispatched (no second approval carrying its id/args). */
static int check_run2(void) {
    int64_t n;

    n = db_count("SELECT COUNT(*) FROM approvals WHERE tool_call_id='call_B'"
                 " AND json_extract(args_json,'$.reason')='doc B';");
    if (n != 1) {
        fprintf(stderr, "FAIL run2: call_B did not park its own approval"
                        " (rows=%lld)\n", (long long)n);
        return 1;
    }

    n = db_count("SELECT COUNT(*) FROM approvals WHERE tool_call_id='call_A';");
    if (n != 1) {
        fprintf(stderr, "FAIL run2: stale call_A re-dispatched — %lld approvals"
                        " carry its tool_call_id\n", (long long)n);
        return 1;
    }

    /* Run 1's request is still open and untouched: run 2 answered its own
     * call, not the oldest pending row. */
    n = db_count("SELECT COUNT(*) FROM approvals WHERE state='pending'"
                 " AND tool_call_id IN ('call_A','call_B');");
    if (n != 2) {
        fprintf(stderr, "FAIL run2: want both requests open, got %lld\n", (long long)n);
        return 1;
    }
    n = db_count("SELECT COUNT(*) FROM tool_calls WHERE call_id='call_B' AND status='done';");
    if (n != 1) {
        fprintf(stderr, "FAIL run2: call_B was not answered\n");
        return 1;
    }

    /* No approval may carry a document that isn't its own call's. */
    n = db_count("SELECT COUNT(*) FROM approvals WHERE"
                 " (tool_call_id='call_A' AND json_extract(args_json,'$.reason')!='doc A')"
                 " OR (tool_call_id='call_B' AND json_extract(args_json,'$.reason')!='doc B');");
    if (n != 0) {
        fprintf(stderr, "FAIL run2: approval carries another call's document\n");
        return 1;
    }
    return 0;
}

/* ── setup ───────────────────────────────────────────────────────── */

static void cleanup_files(void) {
    test_db_clean(DB_PATH);
    unlink(LOG1);
    unlink(LOG2);
    rmdir(DIR_A);
    rmdir(DIR_B);
}

int main(void) {
    TEST_INIT();
    alarm(40);   /* below make's 45s wrapper: fail loud, not hung */

    cleanup_files();
    mkdir(DIR_A, 0755);
    mkdir(DIR_B, 0755);

    char exe[PATH_MAX];
    if (!realpath("build/cclaw", exe)) FAIL("realpath build/cclaw (run from repo root)");

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);

    g_env.url = url;
    int rc = 1;
    if (integ_seed_provider(DB_PATH, url, 0, 0) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* ── run 1: park + left pending ───────────────────────────────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL_A);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if ((g_exit = integ_run_cli_p(&g_env, exe, "please read document A", 0, LOG1, 200)) < 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n");
        integ_dump_log(LOG1);
        goto done;
    }
    if (check_run1()) { integ_dump_log(LOG1); goto done; }

    int64_t sid = db_count("SELECT MAX(id) FROM sessions;");
    if (sid <= 0) { fprintf(stderr, "FAIL: no session after run 1\n"); goto done; }

    /* ── run 2: same session, a different document ────────────────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL_B);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if ((g_exit = integ_run_cli_p(&g_env, exe, "now read document B", sid, LOG2, 200)) < 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        integ_dump_log(LOG2);
        goto done;
    }
    if (check_run2()) { integ_dump_log(LOG2); goto done; }

    printf("All approval park tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
