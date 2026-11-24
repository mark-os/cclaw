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
#include "mock_server.h"
#include "test_integ.h"
#include <limits.h>

#define DB_PATH "/tmp/cclaw_integ_tool_filter.db"
#define LOG1    "/tmp/cclaw_integ_tool_filter_run1.log"
#define LOG2    "/tmp/cclaw_integ_tool_filter_run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

static IntegEnv g_env = {.db = DB_PATH};
#define db_count(sql) integ_db_int(DB_PATH, (sql), NULL)
#define db_exec(sql) integ_db_exec(DB_PATH, (sql))

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
    g_env.url = url;

    int rc = 1;
    if (integ_seed_provider(DB_PATH, url, 0, 0) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* ── run 1: create the session (and the agent + tool rows) ────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&g_env, exe, "hello", 0, LOG1, 200) < 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n");
        integ_dump_log(LOG1);
        goto done;
    }

    int64_t sid = db_count("SELECT MAX(id) FROM sessions;");
    if (sid <= 0) { fprintf(stderr, "FAIL: no session after run 1\n"); goto done; }

    /* Grant the tool at agent level, then narrow THIS session to exclude it:
     * the two authorities must disagree, or we'd be testing the grant gate. */
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO grants(agent_name,kind,value,source)"
        "  SELECT agent_name,'tool','file_write','operator' FROM sessions WHERE id=%lld;"
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
    if (integ_run_cli_p(&g_env, exe, "write a note", sid, LOG2, 200) < 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        integ_dump_log(LOG2);
        goto done;
    }
    if (check_refusal()) { integ_dump_log(LOG2); goto done; }

    printf("All tool filter integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
