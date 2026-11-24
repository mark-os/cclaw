/* A forked tool's result carries the tool's name.
 *
 * The reap path used to store '' in entries.tool_name. Only the Gemini wire
 * format reads the column (it becomes functionResponse.name, and Gemini
 * rejects an empty one with HTTP 400), so every request after a shell_exec,
 * js_eval or file-tool result failed on Gemini and the agent fell back to its
 * next model.
 *
 * The unit harness calls tool handlers directly and writes the result row
 * itself, so it never ran the reap path and could not see this. This drives
 * the real binary: two `-p` runs against a mock LLM, the second calling a
 * forked tool.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "mock_server.h"
#include "test_integ.h"
#include <limits.h>

#define DB_PATH "/tmp/cclaw_integ_tool_result_name.db"
#define LOG1    "/tmp/cclaw_integ_tool_result_name_run1.log"
#define LOG2    "/tmp/cclaw_integ_tool_result_name_run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

static IntegEnv g_env = {.db = DB_PATH};
#define db_count(sql) integ_db_int(DB_PATH, (sql), NULL)
#define db_exec(sql) integ_db_exec(DB_PATH, (sql))

/* Turn 2: call a tool that runs in a forked child. */
static const char *RESP_CALL_FORKED =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_F\",\"type\":\"function\",\"function\":"
    "{\"name\":\"file_list\",\"arguments\":\"{\\\"path\\\":\\\".\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static const char *RESP_FINAL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"acknowledged\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

/* ── helpers ─────────────────────────────────────────────────────── */

/* ── assertions ──────────────────────────────────────────────────── */

static int check_result_named(void) {
    int64_t n;

    /* The call went to the tool, not to a gate that answers inline. */
    n = db_count("SELECT COUNT(*) FROM tool_calls WHERE call_id='call_F'"
                 " AND status='done' AND resolved_by IS NULL;");
    if (n != 1) {
        fprintf(stderr, "FAIL: call_F was not run by the tool (rows=%lld)\n",
                (long long)n);
        return 1;
    }
    n = db_count("SELECT COUNT(*) FROM entries WHERE type='tool_result'"
                 " AND tool_call_id='call_F';");
    if (n != 1) {
        fprintf(stderr, "FAIL: expected one result for call_F (rows=%lld)\n",
                (long long)n);
        return 1;
    }
    n = db_count("SELECT COUNT(*) FROM entries WHERE type='tool_result'"
                 " AND tool_call_id='call_F' AND tool_name='file_list';");
    if (n != 1) {
        fprintf(stderr, "FAIL: the forked tool's result does not name its tool\n");
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

    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO grants(agent_name,kind,value,source)"
        "  SELECT agent_name,'tool','file_list','operator' FROM sessions WHERE id=%lld;",
        (long long)sid);
    if (db_exec(sql) != 0) { fprintf(stderr, "FAIL: could not grant file_list\n"); goto done; }

    /* ── run 2: the forked call ─────────────────────────────────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL_FORKED);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&g_env, exe, "list files", sid, LOG2, 200) < 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        integ_dump_log(LOG2);
        goto done;
    }
    if (check_result_named()) { integ_dump_log(LOG2); goto done; }

    printf("All tool result name integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
