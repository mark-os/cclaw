/* The Anthropic Messages wire format, end to end through the real binary.
 *
 * The unit tests cover the request builder and the response ingest in
 * isolation; what they cannot see is the two meeting across a real tool loop:
 * the URL and headers the transport picks, a forked tool's result landing in
 * the entries the next request is built from, and the thinking block going
 * back where it came from. Two `-p` runs against a mock /v1/messages (which
 * rejects a request missing the version or key header, as the real API does).
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

#define DB_PATH "/tmp/cclaw_integ_anthropic.db"
#define LOG1    "/tmp/cclaw_integ_anthropic_run1.log"
#define LOG2    "/tmp/cclaw_integ_anthropic_run2.log"

#define USAGE "\"usage\":{\"input_tokens\":3,\"cache_read_input_tokens\":100," \
              "\"cache_creation_input_tokens\":20,\"output_tokens\":5}"

static const char *RESP_CALL =
    "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
    "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"SIG-1\"},"
    "{\"type\":\"tool_use\",\"id\":\"toolu_F\",\"name\":\"file_list\","
    " \"input\":{\"path\":\".\"}}],"
    "\"stop_reason\":\"tool_use\"," USAGE "}";

static const char *RESP_FINAL =
    "{\"id\":\"msg_2\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
    "{\"type\":\"text\",\"text\":\"acknowledged\"}],"
    "\"stop_reason\":\"end_turn\"," USAGE "}";

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
static int run_cli_p(const char *exe, const char *prompt, int64_t sid,
                     const char *logpath) {
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
        setenv("ANTHROPIC_API_KEY", "test-key", 1);
        /* Nothing but the providers row may select the wire format. */
        unsetenv("CCLAW_PROVIDER_BASE_URL");
        unsetenv("CCLAW_PROVIDER_ENDPOINT_TYPE");
        unsetenv("CCLAW_MODEL");
        unsetenv("OPENROUTER_API_KEY");
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
        if (waitpid(pid, &st, WNOHANG) == pid) return 0;
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

/* One integer, with an optional text parameter bound at ?1. */
static int64_t db_int(const char *sql, const char *arg) {
    sqlite3 *db;
    if (sqlite3_open(DB_PATH, &db) != SQLITE_OK) return -1;
    sqlite3_stmt *s;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (arg) sqlite3_bind_text(s, 1, arg, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    return v;
}

/* The only provider is an Anthropic-format row pointing at the mock. */
static int seed_mock_provider(const char *url) {
    sqlite3 *seed = test_db_open(DB_PATH);
    if (!seed) return -1;
    if (db_seed_defaults(seed) != 0) { sqlite3_close(seed); return -1; }
    sqlite3_exec(seed, "DELETE FROM models; DELETE FROM providers;", NULL, NULL, NULL);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(seed,
            "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, default_model)"
            " VALUES('anthropic', ?1, 'anthropic', 'ANTHROPIC_API_KEY', 'claude-test')",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, url, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_exec(seed,
        "INSERT INTO models(id, provider_name, model, status)"
        " VALUES('anthropic/claude-test', 'anthropic', 'claude-test', 'healthy')",
        NULL, NULL, NULL);
    sqlite3_close(seed);
    return 0;
}

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static int check_tool_loop(void) {
    CHECK(mock_server_request_count() == 2, "expected exactly two requests (call, final)");
    CHECK(db_int("SELECT COUNT(*) FROM tool_calls WHERE call_id='toolu_F'"
                 " AND status='done' AND resolved_by IS NULL;", NULL) == 1,
          "toolu_F was not run by the tool");
    /* Usage normalized to cache-inclusive: 3 + 100 read + 20 written. */
    CHECK(db_int("SELECT COUNT(*) FROM entries WHERE type='assistant_message'"
                 " AND NOT (usage_in=123 AND cached_tokens=100);", NULL) == 0,
          "usage was not normalized on every assistant entry");

    /* The second request: what the first response turned into. */
    const char *req = mock_server_last_request_body();
    CHECK(req, "no request body captured");
    CHECK(db_int("SELECT json_extract(?1,'$.model')='claude-test'"
                 " AND json_extract(?1,'$.max_tokens') >= 16000"
                 " AND json_type(?1,'$.system')='array'"
                 " AND json_extract(?1,'$.messages[#-1].role')='user'", req) == 1,
          "request envelope is not Messages-shaped");
    CHECK(db_int("SELECT json_extract(?1,'$.messages[#-2].role')='assistant'"
                 " AND json_extract(?1,'$.messages[#-2].content[0].type')='thinking'"
                 " AND json_extract(?1,'$.messages[#-2].content[0].signature')='SIG-1'"
                 " AND json_extract(?1,'$.messages[#-2].content[1].id')='toolu_F'", req) == 1,
          "the thinking block did not go back verbatim ahead of its tool_use");
    CHECK(db_int("SELECT json_extract(?1,'$.messages[#-1].content[0].type')='tool_result'"
                 " AND json_extract(?1,'$.messages[#-1].content[0].tool_use_id')='toolu_F'"
                 " AND json_array_length(?1,'$.messages[#-1].content')=1", req) == 1,
          "the tool result did not answer toolu_F in the following user message");
    CHECK(db_int("SELECT COUNT(*) FROM json_each(?1,'$.tools')"
                 " WHERE json_extract(value,'$.name')='file_list'"
                 "   AND json_extract(value,'$.input_schema.type')='object'", req) == 1,
          "file_list is not declared with an input_schema");
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
    if (!realpath("build/cclaw", exe)) {
        fprintf(stderr, "FAIL: realpath build/cclaw (run from repo root)\n");
        return 1;
    }

    int port = mock_server_start();
    if (port < 0) { fprintf(stderr, "FAIL: mock_server_start\n"); return 1; }
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);

    int rc = 1;
    if (seed_mock_provider(url) != 0) { fprintf(stderr, "FAIL: seed\n"); goto done; }

    /* ── run 1: a plain turn — creates the session, agent and tool rows ── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, "hello", 0, LOG1) != 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n");
        dump_log(LOG1);
        goto done;
    }
    int64_t sid = db_int("SELECT MAX(id) FROM sessions;", NULL);
    if (sid <= 0 || mock_server_request_count() != 1 ||
        db_int("SELECT COUNT(*) FROM entries WHERE type='assistant_message'"
               " AND content='acknowledged';", NULL) != 1) {
        fprintf(stderr, "FAIL: run 1 did not complete a turn over /v1/messages\n");
        dump_log(LOG1);
        goto done;
    }
    printf("  PASS plain_turn\n");

    /* ── run 2: thinking + a forked tool call (file_list is granted by
     * default), then the final answer ── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL);
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, "list files", sid, LOG2) != 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        dump_log(LOG2);
        goto done;
    }
    if (check_tool_loop()) { dump_log(LOG2); goto done; }
    printf("  PASS thinking_tool_loop\n");

    printf("All Anthropic integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
