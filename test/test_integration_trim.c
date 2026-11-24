/* A long turn against a small context: C's trim keeps it going.
 *
 * The agent runs no compaction script: the bundled one, which a fresh DB
 * attaches to the default agent, is switched off — what "rolling" is
 * (decision 5) — so C's trim is all there is.
 *
 * One `-p` turn of many iterations against a mock LLM, each response a big
 * assistant message and a call, the agent's routing entry given a trim
 * threshold a few iterations above its head. The real binary runs the whole
 * path — advance_session's check at the end of every iteration, the trim,
 * the next request built from the trimmed branch — and the turn must finish
 * with the model's answer, not stop or overflow.
 */
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "config.h"
#include "context.h"
#include "mock_server.h"
#include "test_integ.h"
#include <limits.h>

#define DB_PATH "/tmp/cclaw_integ_trim.db"
#define LOG1    "/tmp/cclaw_integ_trim_run1.log"
#define LOG2    "/tmp/cclaw_integ_trim_run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

#define STEPS      12
#define STEP_BYTES 6000          /* ~2400 tokens of assistant text per iteration */

static IntegEnv g_env = {.db = DB_PATH, .max_iter = "40", .verbose = 1};
#define db_count(sql) integ_db_int(DB_PATH, (sql), NULL)

/* No prompt count: a mock's would be fiction, and a usable report's count
 * is the head (context.c, plan_head). */
static const char *RESP_FINAL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"all done\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":5}}";

/* Iteration i: STEP_BYTES of text tagged MARK<i>, and a call to a tool that
 * does not exist (answered inline with an error — no grant, no fork). The
 * usage reports the output honestly and no prompt count (a mock's would be
 * fiction), so every size is the provider's or the estimate. */
static char *step_response(int i) {
    char *text = malloc(STEP_BYTES + 1);
    memset(text, 'z', STEP_BYTES);
    text[STEP_BYTES] = 0;
    char mark[32];
    int n = snprintf(mark, sizeof(mark), "MARK%02d ", i);
    memcpy(text, mark, (size_t)n);
    char *body = malloc(STEP_BYTES + 512);
    snprintf(body, STEP_BYTES + 512,
             "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"%s\","
             "\"tool_calls\":[{\"id\":\"call_%02d\",\"type\":\"function\",\"function\":"
             "{\"name\":\"no_such_tool\",\"arguments\":\"{}\"}}]},"
             "\"finish_reason\":\"tool_calls\"}],"
             "\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":%d}}", text, i,
             TOKENS_OF_BYTES(STEP_BYTES));
    free(text);
    return body;
}

/* The agent's head (system prompt + tools) as the trim estimates it, and a
 * threshold four iterations above it; the target two above. */
static int set_limits(int64_t sid, int *threshold_out) {
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return -1;
    char *agent = session_get_agent_name(db, sid);
    Config *cfg = config_load(db);
    int head = cfg && agent ? context_head_estimate(db, agent, sid, cfg, NULL) : -1;
    config_free(cfg);
    int step = TOKENS_OF_BYTES(STEP_BYTES);
    char sql[512];
    snprintf(sql, sizeof(sql),
             "UPDATE agent_models SET trim_threshold=%d, prune_target=%d"
             " WHERE agent_name='%s' AND purpose='chat';",
             head + 4 * step, head + 2 * step, agent ? agent : "");
    int rc = head > 0 && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK
             && sqlite3_changes(db) > 0 ? 0 : -1;
    snprintf(sql, sizeof(sql),
             "UPDATE agent_extensions SET enabled=0 WHERE agent_name='%s'"
             " AND extension_name='compaction' AND enabled=1;", agent ? agent : "");
    if (rc == 0 && (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK ||
                    sqlite3_changes(db) != 1)) {
        fprintf(stderr, "the default agent does not have the bundled compaction\n");
        rc = -1;
    }
    *threshold_out = head + 4 * step;
    free(agent);
    sqlite3_close(db);
    return rc;
}

static void cleanup_files(void) {
    test_db_clean(DB_PATH);
    unlink(LOG1);
    unlink(LOG2);
}

int main(void) {
    TEST_INIT();
    alarm(42);   /* below make's 45s wrapper: fail loud, not hung */
    cleanup_files();

    char exe[PATH_MAX];
    if (!realpath("build/cclaw", exe)) FAIL("realpath build/cclaw (run from repo root)");
    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    g_env.url = url;

    int rc = 1, threshold = 0;
    char sql[256];
    if (integ_seed_provider(DB_PATH, url, 0, 1000000) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* run 1: the session, the agent and its routing entry */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&g_env, exe, "hello", 0, LOG1, 300) < 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n");
        integ_dump_log(LOG1);
        goto done;
    }
    int64_t sid = db_count("SELECT MAX(id) FROM sessions;");
    if (sid <= 0) { fprintf(stderr, "FAIL: no session after run 1\n"); goto done; }
    if (set_limits(sid, &threshold) != 0) { fprintf(stderr, "FAIL: limits\n"); goto done; }

    /* run 2: the long turn */
    mock_server_reset();
    for (int i = 0; i < STEPS; i++) {
        char *b = step_response(i);
        mock_server_enqueue(200, b);
        free(b);
    }
    mock_server_enqueue(200, RESP_FINAL);
    if (integ_run_cli_p(&g_env, exe, "work through it", sid, LOG2, 300) < 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        integ_dump_log(LOG2);
        goto done;
    }

    /* No script ran: the hook is attached but switched off. */
    FILE *lf = fopen(LOG2, "r");
    char line[1024];
    int hooks = 0;
    while (lf && fgets(line, sizeof(line), lf)) if (strstr(line, "hook ContextThreshold")) hooks++;
    if (lf) fclose(lf);
    if (hooks) {
        fprintf(stderr, "FAIL: a ContextThreshold hook ran\n");
        integ_dump_log(LOG2);
        goto done;
    }
    /* Every iteration was asked for, and the turn ended on the answer. */
    if (mock_server_request_count() != STEPS + 1) {
        fprintf(stderr, "FAIL: %d requests, expected %d\n", mock_server_request_count(),
                STEPS + 1);
        integ_dump_log(LOG2);
        goto done;
    }
    snprintf(sql, sizeof(sql),
             "SELECT content='all done' AND stop_reason=1 FROM entries"
             " WHERE id=(SELECT leaf_id FROM sessions WHERE id=%lld);", (long long)sid);
    if (db_count(sql) != 1) {
        fprintf(stderr, "FAIL: the turn did not end on the model's answer\n");
        integ_dump_log(LOG2);
        goto done;
    }
    /* It trimmed mid-turn, more than once, and said so on the branch. */
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=%lld AND type='system'"
             " AND json_extract(data,'$.source')='" TRIM_SOURCE "'"
             " AND json_extract(data,'$.turn_edits[0].dropped.iterations') > 0;",
             (long long)sid);
    if (db_count(sql) < 2) {
        fprintf(stderr, "FAIL: expected at least two mid-turn trims, got %lld\n",
                (long long)db_count(sql));
        integ_dump_log(LOG2);
        goto done;
    }
    /* The last request no longer carried the first iteration, but did carry
     * the newest one and one note — the running total — saying what went. */
    const char *last = mock_server_last_request_body();
    const char *note = last ? strstr(last, "dropped to fit the context") : NULL;
    if (!last || strstr(last, "MARK00") || !strstr(last, "MARK11") || !note ||
        strstr(note + 1, "dropped to fit the context")) {
        fprintf(stderr, "FAIL: the last request is not the trimmed branch\n");
        goto done;
    }
    /* Originals were kept: the first iteration's message is still a row. */
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=%lld"
             " AND content LIKE 'MARK00%%';", (long long)sid);
    if (db_count(sql) != 1) { fprintf(stderr, "FAIL: an original was deleted\n"); goto done; }
    printf("All trim integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0 && !getenv("KEEP")) cleanup_files();
    return rc;
}
