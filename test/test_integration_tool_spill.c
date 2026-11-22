/* An oversized tool result: the agent gets the head and a note, and the file
 * the note names holds ALL of it.
 *
 * The child that ran the tool spills the full output to a file and sends back
 * the head plus a note naming the file. Two bugs lived here with no test:
 *  - cut at the line limit, head + note was itself over the limit, so the
 *    parent truncated a second time and wrote the head over the full output;
 *  - cut at the byte limit, the parent's own smaller cap dropped the note, so
 *    the agent could not tell anything was missing.
 * Both only exist across the child → parent → context path, so this drives
 * the real binary against a mock LLM.
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_integ_tool_spill.db"
#define LOG1    "/tmp/cclaw_integ_tool_spill_run1.log"
#define LOG2    "/tmp/cclaw_integ_tool_spill_run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

/* 5000 short lines: over the line limit, far under the byte limit. */
static const char *RESP_CALL_LINES =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_L\",\"type\":\"function\",\"function\":"
    "{\"name\":\"js_eval\",\"arguments\":\"{\\\"code\\\":\\\"var a=[];for(var i=1;i<=5000;i++)a.push(i);a.join(String.fromCharCode(10))\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

/* One 200000-character line: over the byte limit, one line. */
static const char *RESP_CALL_BYTES =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"call_B\",\"type\":\"function\",\"function\":"
    "{\"name\":\"js_eval\",\"arguments\":\"{\\\"code\\\":\\\"var s='';for(var i=0;i<200000;i++)s+='a';s\\\"}\"}}]},"
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

/* The stored result for `call_id`: ends with a note naming a file, is under
 * the wire limit, and that file is at least `min_bytes` long and — when
 * `last_line` is given — ends with it (the tail the head does not contain). */
static int check_spilled(const char *call_id, long min_bytes, const char *last_line) {
    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");
    char content[70000] = {0};
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT content FROM entries WHERE type='tool_result'"
                               " AND tool_call_id=?1", -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, call_id, -1, SQLITE_STATIC);
        if (sqlite3_step(s) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(s, 0);
            if (t) snprintf(content, sizeof(content), "%s", t);
        }
        sqlite3_finalize(s);
    }
    sqlite3_close(db);
    size_t len = strlen(content);
    if (len == 0) { fprintf(stderr, "FAIL: no result for %s\n", call_id); return 1; }
    if (len >= 64 * 1024) { fprintf(stderr, "FAIL: %s result is %zu bytes\n", call_id, len); return 1; }
    char *note = strstr(content, "[truncated");
    char *path = note ? strstr(note, "Full output: ") : NULL;
    if (!path) {
        fprintf(stderr, "FAIL: %s result (%zu bytes) carries no note naming the full output\n",
                call_id, len);
        return 1;
    }
    path += strlen("Full output: ");
    char *end = strrchr(path, ']');
    if (!end) FAIL("note is not closed");
    *end = '\0';
    struct stat st;
    if (stat(path, &st) != 0) { fprintf(stderr, "FAIL: %s is missing\n", path); return 1; }
    if (st.st_size < min_bytes) {
        fprintf(stderr, "FAIL: %s: the saved output is %lld bytes, expected at least %ld —"
                        " it holds the head, not the full output\n",
                call_id, (long long)st.st_size, min_bytes);
        return 1;
    }
    if (last_line) {
        char buf[64] = {0};
        FILE *f = fopen(path, "r");
        if (!f) FAIL("open saved output");
        long back = (long)sizeof(buf) - 1;
        fseek(f, st.st_size > back ? -back : -(long)st.st_size, SEEK_END);
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        buf[got] = '\0';
        fclose(f);
        if (!strstr(buf, last_line)) {
            fprintf(stderr, "FAIL: %s: the saved output does not end with %s\n", call_id, last_line);
            return 1;
        }
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
    if (db_exec("INSERT OR IGNORE INTO grants(agent_name,kind,value,source)"
                "  SELECT name,'tool','js_eval','operator' FROM agents;") != 0) {
        fprintf(stderr, "FAIL: could not grant js_eval\n"); goto done;
    }

    /* ── run 2: both oversized calls, one per iteration ───────────── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_CALL_LINES);
    mock_server_enqueue(200, RESP_CALL_BYTES);
    mock_server_enqueue(200, RESP_FINAL);
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "make a lot of output", sid, LOG2) != 0) {
        fprintf(stderr, "FAIL: run 2 did not exit\n");
        dump_log(LOG2);
        goto done;
    }
    if (check_spilled("call_L", 20000, "5000")) { dump_log(LOG2); goto done; }
    printf("  PASS line_limit_keeps_note_and_full_output\n");
    if (check_spilled("call_B", 200000, NULL)) { dump_log(LOG2); goto done; }
    printf("  PASS byte_limit_keeps_note_and_full_output\n");

    printf("All tool spill integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0) cleanup_files();
    return rc;
}
