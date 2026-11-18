/* A long turn against a small context: C's trim keeps it going.
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

#define DB_PATH "/tmp/cclaw_integ_trim.db"
#define LOG1    "/tmp/cclaw_integ_trim_run1.log"
#define LOG2    "/tmp/cclaw_integ_trim_run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

#define STEPS      12
#define STEP_BYTES 6000          /* ~2000 tokens of assistant text per iteration */

static const char *RESP_FINAL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"all done\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

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
             STEP_BYTES / 3);
    free(text);
    return body;
}

static void dump_log(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    fprintf(stderr, "--- %s ---\n", path);
    while (fgets(line, sizeof(line), f)) fputs(line, stderr);
    fprintf(stderr, "--- end %s ---\n", path);
    fclose(f);
}

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
        setenv("CCLAW_MAX_ITERATIONS", "40", 1);
        unsetenv("CCLAW_SANDBOX_PROFILE");
        if (sid > 0) {
            char sbuf[32];
            snprintf(sbuf, sizeof(sbuf), "%lld", (long long)sid);
            execl(exe, exe, "-v", "-p", prompt, "-s", sbuf, (char *)NULL);
        } else {
            execl(exe, exe, "-p", prompt, (char *)NULL);
        }
        _exit(111);
    }
    for (int i = 0; i < 300; i++) {   /* 30s deadline */
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
    if ((db = db_open(DB_PATH)) == NULL) return -1;
    sqlite3_stmt *s;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    return v;
}

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
        "INSERT INTO models(id, provider_name, model, status, context_window)"
        " VALUES('openrouter/deepseek/deepseek-v4-flash', 'openrouter',"
        "        'deepseek/deepseek-v4-flash', 'healthy', 1000000)",
        NULL, NULL, NULL);
    sqlite3_close(seed);
    return 0;
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
    int step = STEP_BYTES / 3;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "UPDATE agent_models SET trim_threshold=%d, prune_target=%d"
             " WHERE agent_name='%s' AND purpose='chat';",
             head + 4 * step, head + 2 * step, agent ? agent : "");
    int rc = head > 0 && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK
             && sqlite3_changes(db) > 0 ? 0 : -1;
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

    int rc = 1, threshold = 0;
    char sql[256];
    if (seed_mock_provider(url) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* run 1: the session, the agent and its routing entry */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "hello", 0, LOG1) != 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n");
        dump_log(LOG1);
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
    if (run_cli_p(exe, url, "work through it", sid, LOG2) != 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        dump_log(LOG2);
        goto done;
    }

    /* Every iteration was asked for, and the turn ended on the answer. */
    if (mock_server_request_count() != STEPS + 1) {
        fprintf(stderr, "FAIL: %d requests, expected %d\n", mock_server_request_count(),
                STEPS + 1);
        dump_log(LOG2);
        goto done;
    }
    snprintf(sql, sizeof(sql),
             "SELECT content='all done' AND stop_reason=1 FROM entries"
             " WHERE id=(SELECT leaf_id FROM sessions WHERE id=%lld);", (long long)sid);
    if (db_count(sql) != 1) {
        fprintf(stderr, "FAIL: the turn did not end on the model's answer\n");
        dump_log(LOG2);
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
        dump_log(LOG2);
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
    printf("  PASS long_turn_trims_mid_turn_and_finishes\n");
    printf("All trim integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0 && !getenv("KEEP")) cleanup_files();
    return rc;
}
