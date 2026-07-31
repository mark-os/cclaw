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
 *   run 1: model calls request_config (call_A, "doc A") → parks → auto-deny
 *   run 2: model calls request_config (call_B, "doc B")
 * Post-fix, call_A is answered in run 1 and call_B parks its own document.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "db.h"
#include "mock_server.h"
#include "test_util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_integ_approval_park.db"
#define LOG1    "/tmp/cclaw_integ_approval_park_run1.log"
#define LOG2    "/tmp/cclaw_integ_approval_park_run2.log"
/* Real directories: a grant path that doesn't exist canonicalizes to its
 * nearest existing ancestor (/tmp), which holds the test DB — and a grant
 * covering cclaw.db is refused before it can ever park. */
#define DIR_A   "/tmp/cclaw_park_a"
#define DIR_B   "/tmp/cclaw_park_b"
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

static void dump_log(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    fprintf(stderr, "--- %s ---\n", path);
    while (fgets(line, sizeof(line), f)) fputs(line, stderr);
    fprintf(stderr, "--- end %s ---\n", path);
    fclose(f);
}

/* Run `cclaw -p <prompt>` (optionally pinned to a session) to completion with
 * stdin on /dev/null, so isatty(stdin) is false — the auto-deny path. */
static int run_cli_p(const char *exe, const char *url, const char *prompt,
                     int64_t sid, const char *logpath) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nfd = open("/dev/null", O_RDONLY);
        int lfd = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (nfd < 0 || lfd < 0) _exit(110);
        dup2(nfd, 0); dup2(lfd, 1); dup2(lfd, 2);
        if (nfd > 2) close(nfd);
        if (lfd > 2) close(lfd);
        setenv("CCLAW_DB_PATH", DB_PATH, 1);
        setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
        setenv("OPENROUTER_API_KEY", "test-key", 1);
        setenv("CCLAW_MODEL", "test-model", 1);
        setenv("CCLAW_STREAM", "0", 1);
        unsetenv("CCLAW_SANDBOX_PROFILE");
        if (sid > 0) {
            char sbuf[32];
            snprintf(sbuf, sizeof(sbuf), "%lld", (long long)sid);
            execl(exe, exe, "-p", prompt, "-s", sbuf, (char *)NULL);
        } else {
            execl(exe, exe, "-p", prompt, (char *)NULL);
        }
        _exit(111);
    }
    for (int i = 0; i < 200; i++) {   /* 20s deadline */
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) return 0;
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* Single-integer query against the test DB. Returns -1 on any failure. */
static int64_t db_count(const char *sql) {
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *s;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    return v;
}

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

    /* The model must be told it was DENIED, not that the request evaporated:
     * "expired without a decision. Not a denial" is what drives the re-request
     * loop. */
    n = db_count("SELECT COUNT(*) FROM entries WHERE role=3"
                 " AND tool_call_id='call_A' AND content LIKE '%denied%';");
    if (n != 1) {
        fprintf(stderr, "FAIL run1: call_A result does not read as a denial\n");
        return 1;
    }
    n = db_count("SELECT COUNT(*) FROM inbox"
                 " WHERE payload LIKE '%expired without a decision%';");
    if (n != 0) {
        fprintf(stderr, "FAIL run1: auto-deny mislabelled as an expiry (rows=%lld)\n",
                (long long)n);
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

/* Point providers/models at the mock, exactly as the request_config PTY test
 * does: the child skips reseeding once the config table is non-empty, so the
 * real OpenRouter rows can't outrank the mock. */
static int seed_mock_provider(const char *url) {
    sqlite3 *seed = test_db_open(DB_PATH);
    if (!seed) return -1;
    if (db_seed_defaults(seed) != 0) { sqlite3_close(seed); return -1; }
    sqlite3_exec(seed, "DELETE FROM models; DELETE FROM providers;", NULL, NULL, NULL);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(seed,
            "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model)"
            " VALUES('mock', ?1, 'openai', 'OPENROUTER_API_KEY', 'test-model')",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, url, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_exec(seed,
        "INSERT INTO models(id, provider_name, model, status, priority)"
        " VALUES('mock/test-model', 'mock', 'test-model', 'healthy', 0)",
        NULL, NULL, NULL);
    sqlite3_close(seed);
    return 0;
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

    int rc = 1;
    if (seed_mock_provider(url) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* ── run 1: park + auto-deny ──────────────────────────────────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL_A);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "please read document A", 0, LOG1) != 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n");
        dump_log(LOG1);
        goto done;
    }
    if (check_run1()) { dump_log(LOG1); goto done; }
    printf("  PASS parked_call_answered_before_exit\n");

    int64_t sid = db_count("SELECT MAX(id) FROM sessions;");
    if (sid <= 0) { fprintf(stderr, "FAIL: no session after run 1\n"); goto done; }

    /* ── run 2: same session, a different document ────────────────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL_B);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "now read document B", sid, LOG2) != 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        dump_log(LOG2);
        goto done;
    }
    if (check_run2()) { dump_log(LOG2); goto done; }
    printf("  PASS fresh_call_parks_its_own_document\n");

    printf("All approval park tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
