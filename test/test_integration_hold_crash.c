/* A process killed while it holds a session for a hook between iterations.
 *
 * The agent has two ContextThreshold hooks: the first leaves a note at the
 * end of the running turn (no "before"), so the note is the session's leaf;
 * the second runs long. The process is killed with SIGKILL while the second
 * runs — the session is 'settling', mid-turn, its leaf a role-0 note.
 *
 *   1. Daemon. A throwaway `cclaw --daemon` takes a message, holds the
 *      session, and is killed. A second daemon on the same DB recovers the
 *      session (dead owner → idle) and resumes the interrupted turn: the
 *      next request goes out as the same turn, and the turn ends on the
 *      model's answer. No message opens a new turn.
 *   2. CLI. A `-p -s` run holds the session the same way and is killed. The
 *      next `-p -s` run recovers it, its prompt opens the next turn after the
 *      interrupted one (the CLI adopts no orphaned work), and it ends on the
 *      model's answer.
 *
 * The mock LLM answers; no live call. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "config.h"
#include "config_registry.h"
#include "context.h"
#include "db.h"
#include "extension_manifest.h"
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

#define ROOT    "/tmp/cclaw_integ_holdcrash"
#define DB_PATH ROOT "/cclaw.db"
#define WS      ROOT "/ws"
#define HOME    ROOT "/home"
#define BUNDLE  ROOT "/bundle"
#define LOG1    ROOT "/cli.log"
#define LOGD    ROOT "/daemon.log"

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); goto done; } } while (0)

#define STEP_BYTES 6000

/* No prompt count: a mock's would be fiction, and a usable report's count
 * is the head (context.c, plan_head). */
static const char *RESP_FINAL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"all done\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":5}}";

/* One iteration: STEP_BYTES of text and a call to a tool that does not exist
 * (answered inline with an error — no grant, no fork). */
static char *step_response(void) {
    char *text = malloc(STEP_BYTES + 1);
    memset(text, 'z', STEP_BYTES);
    text[STEP_BYTES] = 0;
    char *body = malloc(STEP_BYTES + 512);
    snprintf(body, STEP_BYTES + 512,
             "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"%s\","
             "\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\",\"function\":"
             "{\"name\":\"no_such_tool\",\"arguments\":\"{}\"}}]},"
             "\"finish_reason\":\"tool_calls\"}],"
             "\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":%d}}", text,
             TOKENS_OF_BYTES(STEP_BYTES));
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

static void child_env(const char *url) {
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    setenv("CCLAW_WORKSPACE", WS, 1);
    setenv("HOME", HOME, 1);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_SANDBOX_PROFILE", "host", 1);
}

/* A `-p` run, started and left running (the caller waits or kills it). */
static pid_t spawn_cli(const char *exe, const char *url, const char *prompt, int64_t sid) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int nfd = open("/dev/null", O_RDONLY);
        int lfd = open(LOG1, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (nfd < 0 || lfd < 0) _exit(110);
        dup2(nfd, 0); dup2(lfd, 1); dup2(lfd, 2);
        child_env(url);
        char sbuf[32];
        snprintf(sbuf, sizeof(sbuf), "%lld", (long long)sid);
        if (sid > 0) execl(exe, exe, "-v", "-p", prompt, "-s", sbuf, (char *)NULL);
        else execl(exe, exe, "-p", prompt, (char *)NULL);
        _exit(111);
    }
    return pid;
}

static int wait_exit(pid_t pid, int tenths) {
    for (int i = 0; i < tenths; i++) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128;
        usleep(100 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -1;
}

static pid_t spawn_daemon(const char *exe, const char *url) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open(LOGD, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        child_env(url);
        execl(exe, exe, "-v", "--daemon", (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void stop(pid_t pid, int sig) {
    if (pid <= 0) return;
    kill(pid, sig);
    for (int i = 0; i < 100 && waitpid(pid, NULL, WNOHANG) == 0; i++) usleep(100000);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static int64_t db_int(const char *sql) {
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return -1;
    sqlite3_stmt *s;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
        sqlite3_finalize(s);
    }
    db_close(db);
    return v;
}

static int db_exec(const char *sql) {
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return -1;
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) fprintf(stderr, "sql: %s\n  %s\n", err ? err : "?", sql);
    sqlite3_free(err);
    db_close(db);
    return rc == SQLITE_OK ? 0 : -1;
}

/* Poll until `sql` returns 1. */
static int until(const char *sql, int tenths) {
    for (int i = 0; i < tenths; i++) {
        if (db_int(sql) == 1) return 1;
        usleep(100 * 1000);
    }
    return 0;
}

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
        "INSERT INTO models(id, provider_name, model, status, context_window)"
        " VALUES('openrouter/deepseek/deepseek-v4.1-flash', 'openrouter',"
        "        'deepseek/deepseek-v4.1-flash', 'healthy', 1000000)",
        NULL, NULL, NULL);
    sqlite3_close(seed);
    return 0;
}

/* Over the threshold after one iteration; the bundled compaction off; the
 * hooks' extension attached; operator notes to an agent with no session. */
static int configure(int64_t sid, char *agent_out, size_t cap) {
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return -1;
    char *agent = session_get_agent_name(db, sid);
    Config *cfg = config_load(db);
    int head = cfg && agent ? context_head_estimate(db, agent, sid, cfg, NULL) : -1;
    config_free(cfg);
    char sql[640];
    snprintf(sql, sizeof(sql),
             "UPDATE agent_models SET trim_threshold=%d, prune_target=%d"
             " WHERE agent_name='%s' AND purpose='chat';"
             "UPDATE agent_extensions SET enabled=0 WHERE agent_name='%s'"
             " AND extension_name='compaction';"
             "UPDATE agents SET sandbox_profile='host' WHERE name='%s';",
             head + STEP_BYTES / 6, head + STEP_BYTES / 12, agent, agent, agent);
    int rc = head > 0 && agent && sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK
             ? 0 : -1;
    test_seed_agent(db, "Nobody");
    if (rc == 0) rc = config_set(db, "default_agent", "Nobody");
    snprintf(agent_out, cap, "%s", agent ? agent : "");
    free(agent);
    if (rc == 0) {
        FILE *f = fopen(BUNDLE "/extension.json", "w");
        if (!f) rc = -1;
        else {
            fputs("{\"name\":\"hc\",\"version\":\"1.0.0\",\"hooks\":["
                  "{\"event\":\"ContextThreshold\",\"handler\":\"note.qjs\"},"
                  "{\"event\":\"ContextThreshold\",\"handler\":\"slow.qjs\",\"timeout\":8}]}", f);
            fclose(f);
        }
        f = fopen(BUNDLE "/note.qjs", "w");
        if (f) {
            fputs("return JSON.stringify({edits:[{op:'note',text:'HOLD NOTE'}]});", f);
            fclose(f);
        }
        f = fopen(BUNDLE "/slow.qjs", "w");
        if (f) {
            fputs("var t = Date.now(); while (Date.now() - t < 30000) {} return '{}';", f);
            fclose(f);
        }
        char *err = NULL;
        if (!f || extension_install(db, BUNDLE, agent_out, 0, &err) != 0) {
            fprintf(stderr, "install: %s\n", err ? err : "?");
            rc = -1;
        }
        free(err);
    }
    db_close(db);
    return rc;
}

/* The session held mid-turn, its leaf the first hook's note. */
static void held_sql(char *sql, size_t cap, int64_t sid) {
    snprintf(sql, cap,
             "SELECT s.state='settling' AND s.turn_iteration > 0 AND e.type='system'"
             " AND e.content='HOLD NOTE' FROM sessions s JOIN entries e ON e.id=s.leaf_id"
             " WHERE s.id=%lld", (long long)sid);
}

/* The turn after `since` ended on the model's answer; `turns` turns were
 * opened after `since`. */
static int answered(int64_t sid, int64_t since, int turns) {
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT s.state='idle' AND e.content='all done' AND e.stop_reason=1"
             " AND (SELECT COUNT(DISTINCT turn_id) FROM entries WHERE session_id=%lld"
             "      AND id > %lld) = %d"
             " FROM sessions s JOIN entries e ON e.id=s.leaf_id WHERE s.id=%lld",
             (long long)sid, (long long)since, turns, (long long)sid);
    return until(sql, 150);
}

int main(void) {
    TEST_INIT();
    alarm(42);
    printf("test_integration_hold_crash:\n");
    (void)!system("rm -rf " ROOT);
    mkdir(ROOT, 0755);
    mkdir(WS, 0755);
    mkdir(HOME, 0755);
    mkdir(BUNDLE, 0755);

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
    pid_t daemon = 0, cli = 0;
    char *step = NULL;
    char agent[64] = "", sql[512];
    CHECK(seed_mock_provider(url) == 0, "seed");

    /* The session, its agent and routing entry. */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    cli = spawn_cli(exe, url, "hello", 0);
    if (wait_exit(cli, 150) != 0) { dump_log(LOG1); CHECK(0, "run 1 did not exit"); }
    cli = 0;
    int64_t sid = db_int("SELECT MAX(id) FROM sessions");
    CHECK(sid > 0, "no session after run 1");
    CHECK(configure(sid, agent, sizeof(agent)) == 0, "configure");

    /* ── 1. daemon ── */
    step = step_response();
    mock_server_reset();
    mock_server_enqueue(200, step);
    mock_server_enqueue(200, RESP_FINAL);
    int64_t since = db_int("SELECT MAX(id) FROM entries");
    snprintf(sql, sizeof(sql), "INSERT INTO inbox(session_id, source, payload)"
             " VALUES(%lld, 'cli', 'go on')", (long long)sid);
    CHECK(db_exec(sql) == 0, "queue the message");
    daemon = spawn_daemon(exe, url);
    CHECK(daemon > 0, "spawn the daemon");
    held_sql(sql, sizeof(sql), sid);
    if (!until(sql, 150)) dump_log(LOGD);
    CHECK(until(sql, 1), "the daemon did not hold the session with the note as its leaf");
    stop(daemon, SIGKILL);
    daemon = 0;
    CHECK(mock_server_request_count() == 1, "the killed daemon sent more than one request");
    daemon = spawn_daemon(exe, url);
    CHECK(daemon > 0, "respawn the daemon");
    if (!answered(sid, since, 1)) dump_log(LOGD);
    CHECK(answered(sid, since, 1), "the interrupted turn was not resumed to its answer");
    CHECK(mock_server_request_count() == 2, "the resumed turn did not make one request");
    {
        const char *last = mock_server_last_request_body();
        CHECK(last && strstr(last, "HOLD NOTE") && strstr(last, "go on"),
              "the resumed request does not carry the turn and its note");
    }
    stop(daemon, SIGTERM);
    daemon = 0;
    printf("  PASS daemon_resumes_turn_held_at_crash\n");

    /* ── 2. CLI ── */
    mock_server_reset();
    mock_server_enqueue(200, step);
    mock_server_enqueue(200, RESP_FINAL);
    since = db_int("SELECT MAX(id) FROM entries");
    cli = spawn_cli(exe, url, "once more", sid);
    held_sql(sql, sizeof(sql), sid);
    if (!until(sql, 150)) dump_log(LOG1);
    CHECK(until(sql, 1), "the CLI did not hold the session with the note as its leaf");
    stop(cli, SIGKILL);
    cli = 0;
    cli = spawn_cli(exe, url, "and now?", sid);
    int st = wait_exit(cli, 200);
    cli = 0;
    if (st != 0) dump_log(LOG1);
    CHECK(st == 0, "the next CLI run did not finish");
    CHECK(answered(sid, since, 2), "the next CLI run did not end its turn on the answer");
    printf("  PASS cli_next_turn_after_crash\n");

    printf("All hold-crash integration tests passed.\n");
    rc = 0;
done:
    free(step);
    stop(daemon, SIGKILL);
    stop(cli, SIGKILL);
    mock_server_stop();
    if (rc == 0) (void)!system("rm -rf " ROOT);
    return rc;
}
