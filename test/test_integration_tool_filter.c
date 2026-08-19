/* The session tool_filter refusal must teach the route-around.
 *
 * Regression source (plan/projects/blocking-vs-background-tools.md, defect 3;
 * CharlesDow experiment 2026-07-31): a self-spawned worker called a tool its
 * grants covered but its spawn-frozen filter excluded. The refusal read
 * "blocked by this session's tool filter" and stopped there — the model had to
 * *infer* that a filter is a spawn-time choice belonging to its parent. The
 * message now names the fix, so the worker can relay it upward.
 *
 * The dispatch gate lives in main.c and has no in-process seam (the unit
 * harness calls handlers directly, bypassing it), so this drives the real
 * binary: two `-p` runs against a mock LLM, with the filter installed on the
 * session between them.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "db.h"
#include "mock_server.h"
#include "test_util.h"
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_integ_tool_filter.db"
#define LOG1    "/tmp/cclaw_integ_tool_filter_run1.log"
#define LOG2    "/tmp/cclaw_integ_tool_filter_run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

/* Turn 2: call a tool the agent is granted but the session filter excludes. */
static const char *RESP_CALL_FILTERED =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_F\",\"type\":\"function\",\"function\":"
    "{\"name\":\"file_write\",\"arguments\":"
    "\"{\\\"path\\\":\\\"note.txt\\\",\\\"content\\\":\\\"hi\\\"}\"}}]},"
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

/* Run `cclaw -p <prompt>` (optionally pinned to a session), stdin /dev/null. */
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

static int db_exec(const char *sql) {
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return -1;
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL);
    sqlite3_close(db);
    return (rc == SQLITE_OK) ? 0 : -1;
}

/* Point providers/models at the mock (same shape as the approval-park test). */
static int seed_mock_provider(const char *url) {
    sqlite3 *seed = test_db_open(DB_PATH);
    if (!seed) return -1;
    if (db_seed_defaults(seed) != 0) { sqlite3_close(seed); return -1; }
    sqlite3_exec(seed, "DELETE FROM models; DELETE FROM providers;", NULL, NULL, NULL);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(seed,
            "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model)"
            " VALUES('openrouter', ?1, 'openai', 'OPENROUTER_API_KEY', 'deepseek/deepseek-v4-flash')",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, url, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_exec(seed,
        "INSERT INTO models(id, provider_name, model, status)"
        " VALUES('openrouter/deepseek/deepseek-v4-flash', 'openrouter',"
        "        'deepseek/deepseek-v4-flash', 'healthy')",
        NULL, NULL, NULL);
    sqlite3_close(seed);
    return 0;
}

/* ── assertions ──────────────────────────────────────────────────── */

static int check_refusal(void) {
    int64_t n;

    n = db_count("SELECT COUNT(*) FROM tool_calls WHERE call_id='call_F'"
                 " AND status='done' AND resolved_by='tool_filter';");
    if (n != 1) {
        fprintf(stderr, "FAIL: call_F not resolved by the tool filter"
                        " (matching rows=%lld)\n", (long long)n);
        return 1;
    }

    /* The wall … */
    n = db_count("SELECT COUNT(*) FROM entries WHERE role=3"
                 " AND tool_call_id='call_F'"
                 " AND content LIKE '%blocked by this session''s tool filter%';");
    if (n != 1) {
        fprintf(stderr, "FAIL: no tool_filter refusal result for call_F"
                        " (rows=%lld)\n", (long long)n);
        return 1;
    }

    /* … and the way around it: a filter is the *spawner's* choice, and the
     * fix is a wider `tools` array, not a grant request. */
    n = db_count("SELECT COUNT(*) FROM entries WHERE role=3"
                 " AND tool_call_id='call_F'"
                 " AND content LIKE '%narrowed toolset%'"
                 " AND content LIKE '%spawner can pass tools:%'"
                 " AND content LIKE '%within its own grants%';");
    if (n != 1) {
        fprintf(stderr, "FAIL: refusal does not teach the route-around\n");
        return 1;
    }

    /* Not a grant problem — the grant is present, so the model must not be
     * told to go ask for one. */
    n = db_count("SELECT COUNT(*) FROM entries WHERE role=3"
                 " AND tool_call_id='call_F' AND content LIKE '%not granted%';");
    if (n != 0) {
        fprintf(stderr, "FAIL: refusal misreports a filtered tool as ungranted\n");
        return 1;
    }
    return 0;
}

static void cleanup_files(void) {
    test_db_clean(DB_PATH);
    unlink(LOG1);
    unlink(LOG2);
}

int main(void) {
    TEST_INIT();
    alarm(40);   /* below make's 45s wrapper: fail loud, not hung */

    cleanup_files();

    char exe[PATH_MAX];
    if (!realpath("build/cclaw", exe)) FAIL("realpath build/cclaw (run from repo root)");

    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);

    int rc = 1;
    if (seed_mock_provider(url) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* ── run 1: create the session (and the agent + tool rows) ────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "hello", 0, LOG1) != 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n");
        dump_log(LOG1);
        goto done;
    }

    int64_t sid = db_count("SELECT MAX(id) FROM sessions;");
    if (sid <= 0) { fprintf(stderr, "FAIL: no session after run 1\n"); goto done; }

    /* Grant the tool at agent level, then narrow THIS session to exclude it:
     * the two authorities must disagree, or we'd be testing the grant gate. */
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO grants(agent_name,kind,value)"
        "  SELECT agent_name,'tool','file_write' FROM sessions WHERE id=%lld;"
        "UPDATE sessions SET tool_filter='[\"file_read\"]' WHERE id=%lld;",
        (long long)sid, (long long)sid);
    if (db_exec(sql) != 0) { fprintf(stderr, "FAIL: could not install filter\n"); goto done; }
    if (db_count("SELECT COUNT(*) FROM grants g JOIN sessions s ON s.agent_name=g.agent_name"
                 " WHERE s.id=" "(SELECT MAX(id) FROM sessions)"
                 "   AND g.kind='tool' AND g.value='file_write';") < 1) {
        fprintf(stderr, "FAIL: file_write grant not installed —"
                        " the test would be exercising the grant gate, not the filter\n");
        goto done;
    }

    /* ── run 2: the filtered call ─────────────────────────────────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL_FILTERED);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "write a note", sid, LOG2) != 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        dump_log(LOG2);
        goto done;
    }
    if (check_refusal()) { dump_log(LOG2); goto done; }
    printf("  PASS tool_filter_refusal_teaches_the_route_around\n");

    printf("All tool filter integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
