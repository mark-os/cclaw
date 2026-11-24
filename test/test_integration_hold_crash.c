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
#include "extension_manifest.h"
#include "mock_server.h"
#include "test_integ.h"
#include <limits.h>
#include <sys/stat.h>

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

static IntegEnv g_env = {.db = DB_PATH, .workspace = WS, .home = HOME,
                         .containment = "none", .verbose = 1};
#define db_int(sql) integ_db_int(DB_PATH, (sql), NULL)
#define db_exec(sql) integ_db_exec(DB_PATH, (sql))

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

/* Poll until `sql` returns 1. */
static int until(const char *sql, int tenths) {
    for (int i = 0; i < tenths; i++) {
        if (db_int(sql) == 1) return 1;
        usleep(100 * 1000);
    }
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
             "UPDATE agents SET containment='none' WHERE name='%s';",
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
    g_env.url = url;

    int rc = 1;
    pid_t daemon = 0, cli = 0;
    char *step = NULL;
    char agent[64] = "", sql[512];
    CHECK(integ_seed_provider(DB_PATH, url, 0, 1000000) == 0, "seed");

    /* The session, its agent and routing entry. */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    cli = integ_spawn_cli(&g_env, exe, "hello", 0, LOG1, 1);
    if (integ_wait(cli, 150) != 0) { integ_dump_log(LOG1); CHECK(0, "run 1 did not exit"); }
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
    daemon = integ_spawn_daemon(&g_env, exe, LOGD, 1);
    CHECK(daemon > 0, "spawn the daemon");
    held_sql(sql, sizeof(sql), sid);
    if (!until(sql, 150)) integ_dump_log(LOGD);
    CHECK(until(sql, 1), "the daemon did not hold the session with the note as its leaf");
    integ_stop(daemon, SIGKILL);
    daemon = 0;
    CHECK(mock_server_request_count() == 1, "the killed daemon sent more than one request");
    daemon = integ_spawn_daemon(&g_env, exe, LOGD, 1);
    CHECK(daemon > 0, "respawn the daemon");
    if (!answered(sid, since, 1)) integ_dump_log(LOGD);
    CHECK(answered(sid, since, 1), "the interrupted turn was not resumed to its answer");
    CHECK(mock_server_request_count() == 2, "the resumed turn did not make one request");
    {
        const char *last = mock_server_last_request_body();
        CHECK(last && strstr(last, "HOLD NOTE") && strstr(last, "go on"),
              "the resumed request does not carry the turn and its note");
    }
    integ_stop(daemon, SIGTERM);
    daemon = 0;

    /* ── 2. CLI ── */
    mock_server_reset();
    mock_server_enqueue(200, step);
    mock_server_enqueue(200, RESP_FINAL);
    since = db_int("SELECT MAX(id) FROM entries");
    cli = integ_spawn_cli(&g_env, exe, "once more", sid, LOG1, 1);
    held_sql(sql, sizeof(sql), sid);
    if (!until(sql, 150)) integ_dump_log(LOG1);
    CHECK(until(sql, 1), "the CLI did not hold the session with the note as its leaf");
    integ_stop(cli, SIGKILL);
    cli = 0;
    cli = integ_spawn_cli(&g_env, exe, "and now?", sid, LOG1, 1);
    int st = integ_wait(cli, 200);
    cli = 0;
    if (st != 0) integ_dump_log(LOG1);
    CHECK(st == 0, "the next CLI run did not finish");
    CHECK(answered(sid, since, 2), "the next CLI run did not end its turn on the answer");

    printf("All hold-crash integration tests passed.\n");
    rc = 0;
done:
    free(step);
    integ_stop(daemon, SIGKILL);
    integ_stop(cli, SIGKILL);
    mock_server_stop();
    if (rc == 0) (void)!system("rm -rf " ROOT);
    return rc;
}
