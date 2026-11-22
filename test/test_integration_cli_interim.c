/* The terminal shows every assistant message of a turn, once.
 *
 * A turn used to print only its last message. Text the model wrote alongside
 * tool calls — often the answer itself — was stored and never shown. This
 * drives the real binary against a mock LLM:
 *  - text + a tool call, then a final text: both are on stdout, in order,
 *    each once;
 *  - text + a tool call, then an EMPTY final: the earlier text is on stdout
 *    exactly once (the turn-end fallback must not print it again).
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

#define DB_PATH "/tmp/cclaw_integ_cli_interim.db"
#define LOG1    "/tmp/cclaw_integ_cli_interim_run1.log"
#define LOG2    "/tmp/cclaw_integ_cli_interim_run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

#define CALL(id, text) \
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"" text "\"," \
    "\"tool_calls\":[{\"id\":\"" id "\",\"type\":\"function\",\"function\":" \
    "{\"name\":\"file_list\",\"arguments\":\"{\\\"path\\\":\\\".\\\"}\"}}]}," \
    "\"finish_reason\":\"tool_calls\"}]," \
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}"
#define SAY(text) \
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"" text "\"}," \
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}"

static const char *RESP_FINAL = SAY("acknowledged");

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

static int db_exec(const char *sql) {
    sqlite3 *db;
    if ((db = db_open(DB_PATH)) == NULL) return -1;
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
            " VALUES('openrouter', ?1, 'openai', 'OPENROUTER_API_KEY', 'deepseek/deepseek-v4.1-flash')",
            -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, url, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }
    sqlite3_exec(seed,
        "INSERT INTO models(id, provider_name, model, status)"
        " VALUES('openrouter/deepseek/deepseek-v4.1-flash', 'openrouter',"
        "        'deepseek/deepseek-v4.1-flash', 'healthy')",
        NULL, NULL, NULL);
    sqlite3_close(seed);
    return 0;
}

/* ── assertions ──────────────────────────────────────────────────── */

static int count_in_file(const char *path, const char *needle, long *first_at) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    int c = 0;
    if (first_at) *first_at = -1;
    for (char *p = buf; (p = strstr(p, needle)); p += strlen(needle)) {
        if (c == 0 && first_at) *first_at = (long)(p - buf);
        c++;
    }
    return c;
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

    /* ── setup run: create the session, agent and tool rows ───────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "hello", 0, LOG1) != 0) {
        fprintf(stderr, "FAIL: setup run did not exit\n"); dump_log(LOG1); goto done;
    }
    int64_t sid = db_count("SELECT MAX(id) FROM sessions;");
    if (sid <= 0) { fprintf(stderr, "FAIL: no session\n"); goto done; }
    if (db_exec("INSERT OR IGNORE INTO grants(agent_name,kind,value,source)"
                "  SELECT name,'tool','file_list','operator' FROM agents;") != 0) {
        fprintf(stderr, "FAIL: grant\n"); goto done;
    }

    /* ── text with a tool call, then a final text ─────────────────── */
    mock_server_reset();
    mock_server_enqueue(200, CALL("call_1", "INTERIM-ANSWER-391"));
    mock_server_enqueue(200, SAY("FINAL-WORD"));
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "question", sid, LOG1) != 0) {
        fprintf(stderr, "FAIL: run 1 did not exit\n"); dump_log(LOG1); goto done;
    }
    long at_i, at_f, at_tool;
    int ni = count_in_file(LOG1, "INTERIM-ANSWER-391", &at_i);
    int nf = count_in_file(LOG1, "FINAL-WORD", &at_f);
    count_in_file(LOG1, "[file_list", &at_tool);
    if (ni != 1 || nf != 1) {
        fprintf(stderr, "FAIL: interim shown %d time(s), final %d (want 1 and 1)\n", ni, nf);
        dump_log(LOG1); goto done;
    }
    if (!(at_i < at_tool && at_tool < at_f)) {
        fprintf(stderr, "FAIL: order is not interim text, tool line, final text\n");
        dump_log(LOG1); goto done;
    }
    printf("  PASS interim_text_is_shown_before_its_tool_call\n");

    /* ── text with a tool call, then an empty final ───────────────── */
    mock_server_reset();
    mock_server_enqueue(200, CALL("call_2", "ONLY-THING-SAID"));
    mock_server_enqueue(200, SAY(""));
    mock_server_enqueue(200, SAY(""));
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "another", sid, LOG2) != 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n"); dump_log(LOG2); goto done;
    }
    int no = count_in_file(LOG2, "ONLY-THING-SAID", NULL);
    if (no != 1) {
        fprintf(stderr, "FAIL: the only message was shown %d time(s), want 1\n", no);
        dump_log(LOG2); goto done;
    }
    printf("  PASS empty_final_does_not_repeat_the_earlier_text\n");

    printf("All CLI interim-text integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
