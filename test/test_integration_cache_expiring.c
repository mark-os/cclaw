/* CacheExpiring end to end, through the real binary.
 *
 *   1. One `-p` turn against a mock Anthropic /v1/messages: the agent's
 *      Anthropic entry got the 1-hour default, every cache marker in the
 *      request asks for the hour, and the session is armed from the moment
 *      its request started.
 *   2. A throwaway `cclaw --daemon` on the same DB, after the test has moved
 *      that arm 3300 seconds into the past (the clock is a row, so nothing
 *      sleeps through an hour): the daemon fires the agent's CacheExpiring
 *      hook on its first turn of the loop, the hook's edit lands, the session
 *      is disarmed and idle, and no C trim follows although the session is
 *      over its trim threshold. A second session armed a month ago — what a
 *      daemon finds after a long stop — is disarmed and never fired.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 600
#include "context.h"
#include "extension_manifest.h"
#include "mock_server.h"
#include "test_integ.h"
#include <limits.h>
#include <sys/stat.h>

#define ROOT    "/tmp/cclaw_integ_cacheexp"
#define DB_PATH ROOT "/cclaw.db"
#define WS      ROOT "/ws"
#define HOME    ROOT "/home"
#define BUNDLE  ROOT "/bundle"
#define LOG1    ROOT "/run1.log"
#define LOGD    ROOT "/daemon.log"

#define CHECK(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); goto done; } } while (0)

static const IntegEnv ENV = {.db = DB_PATH, .workspace = WS, .home = HOME,
                             .sandbox = "host"};
#define db_int(sql, arg) integ_db_int(DB_PATH, (sql), (arg))
#define db_exec(sql) integ_db_exec(DB_PATH, (sql))

static const char *RESP_FINAL =
    "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
    "{\"type\":\"text\",\"text\":\"acknowledged\"}],\"stop_reason\":\"end_turn\","
    "\"usage\":{\"input_tokens\":3,\"cache_read_input_tokens\":0,"
    "\"cache_creation_input_tokens\":900,\"output_tokens\":5,"
    "\"cache_creation\":{\"ephemeral_5m_input_tokens\":0,"
    "\"ephemeral_1h_input_tokens\":900}}}";

/* The agent routes to the mock's Anthropic entry, inserted with nothing
 * said, so the schema's default for an Anthropic entry applies. */
static int seed_mock_provider(const char *url) {
    if (integ_seed_provider(DB_PATH, url, 1, 200000) != 0) return -1;
    sqlite3 *seed = db_open(DB_PATH);
    if (!seed) return -1;
    sqlite3_exec(seed, "DELETE FROM agent_models;", NULL, NULL, NULL);
    test_seed_agent(seed, "Assistant");
    int rc = sqlite3_exec(seed,
        "DELETE FROM agent_models;"
        "INSERT INTO agent_models(agent_name, model_id, pos)"
        " VALUES('Assistant', 'anthropic/claude-test', 0);",
        NULL, NULL, NULL);
    db_close(seed);
    return rc == SQLITE_OK ? 0 : -1;
}

/* The agent's CacheExpiring hook: a note naming the numbers it was sent. */
static int install_hook(const char *agent) {
    mkdir(BUNDLE, 0755);
    FILE *f = fopen(BUNDLE "/extension.json", "w");
    if (!f) return -1;
    fputs("{\"name\":\"ce\",\"version\":\"1.0.0\",\"hooks\":["
          "{\"event\":\"CacheExpiring\",\"handler\":\"h.qjs\"}]}", f);
    fclose(f);
    f = fopen(BUNDLE "/h.qjs", "w");
    if (!f) return -1;
    fputs("var c = args.context;"
          "if (c.cache_lifetime !== 3600 || c.depth !== 0 || !(c.tokens > c.prune_target)"
          "    || c.cache_write_tokens !== 900)"
          "  throw new Error('unexpected ' + JSON.stringify(c));"
          "return JSON.stringify({edits:[{op:'note',text:'cache note ' + c.tokens}]});", f);
    fclose(f);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return -1;
    char *err = NULL;
    int rc = extension_install(db, BUNDLE, agent, 0, &err);
    if (rc != 0) fprintf(stderr, "install: %s\n", err ? err : "?");
    free(err);
    db_close(db);
    return rc;
}

int main(void) {
    TEST_INIT();
    alarm(40);
    printf("test_integration_cache_expiring:\n");
    (void)!system("rm -rf " ROOT);
    mkdir(ROOT, 0755);
    mkdir(WS, 0755);
    mkdir(HOME, 0755);

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
    pid_t daemon = 0;
    char *agent = NULL;
    char sql[512];
    CHECK(seed_mock_provider(url) == 0, "seed");

    /* ── 1. the marker and the arm ── */
    mock_server_reset();
    mock_server_enqueue(200, RESP_FINAL);
    int64_t t0 = (int64_t)time(NULL);
    if (integ_run_cli_p(&ENV, exe, "hello", 0, LOG1, 150) < 0) { integ_dump_log(LOG1); CHECK(0, "run 1 did not exit"); }
    int64_t t1 = (int64_t)time(NULL);
    int64_t sid = db_int("SELECT MAX(id) FROM sessions", NULL);
    if (sid <= 0 || mock_server_request_count() != 1) integ_dump_log(LOG1);
    CHECK(sid > 0 && mock_server_request_count() == 1, "run 1 did not make one request");
    {
        sqlite3 *db = db_open(DB_PATH);
        agent = session_get_agent_name(db, sid);
        db_close(db);
    }
    CHECK(agent, "the session has no agent");
    CHECK(db_int("SELECT cache_lifetime FROM agent_models WHERE agent_name=?1"
                 " AND purpose='chat' AND pos=0", agent) == 3600,
          "the Anthropic entry did not default to the 1-hour cache");
    const char *req = mock_server_last_request_body();
    CHECK(req, "no request captured");
    CHECK(db_int("SELECT count(*) >= 2 AND count(*) = sum(json_extract(value,'$.ttl') = '1h')"
                 " AND sum(json_extract(value,'$.type') = 'ephemeral') = count(*)"
                 " FROM json_tree(?1) WHERE key = 'cache_control'", req) == 1,
          "not every cache marker in the request asks for the hour");
    snprintf(sql, sizeof(sql), "SELECT cache_armed_at BETWEEN %lld AND %lld FROM sessions"
             " WHERE id=%lld", (long long)t0, (long long)t1, (long long)sid);
    CHECK(db_int(sql, NULL) == 1, "the session is not armed from its request's start");
    CHECK(db_int("SELECT cache_write_tokens FROM llm_responses WHERE status='ok'"
                 " ORDER BY id DESC LIMIT 1", NULL) == 900,
          "the 1-hour write did not land in cache_write_tokens");

    /* ── 2. a daemon fires it once, on the session whose cache is about to
     * lapse, and on no other ── */
    CHECK(install_hook(agent) == 0, "install the hook");
    snprintf(sql, sizeof(sql),
             "UPDATE agent_extensions SET enabled=0 WHERE agent_name='%s'"
             " AND extension_name='compaction';"
             "UPDATE agents SET sandbox_profile='host' WHERE name='%s';"
             /* Over even its trim threshold: a C check after the hook would
              * trim it, and must not run. */
             "UPDATE agent_models SET trim_threshold=40, prune_target=20"
             " WHERE agent_name='%s' AND purpose='chat';"
             "UPDATE sessions SET cache_armed_at=unixepoch()-3300 WHERE id=%lld;",
             agent, agent, agent, (long long)sid);
    CHECK(db_exec(sql) == 0, "arm the session into the past");
    CHECK(db_exec("INSERT OR REPLACE INTO config(key, value) VALUES('hook_timeout','20');") == 0,
          "hook_timeout");
    int64_t lapsed;
    {
        sqlite3 *db = db_open(DB_PATH);
        lapsed = session_create(db, "old", agent, -1, 0);
        Message u = {.role = ROLE_USER, .content = "an old conversation"};
        entry_append_with_iteration(db, lapsed, &u, 0);
        entry_append_typed(db, lapsed, db_next_iteration_id(db, lapsed), "assistant_message",
                           0, "answered long ago", NULL, NULL, 0, STOP_REASON_NONE, NULL,
                           0, 0, 0);
        snprintf(sql, sizeof(sql), "UPDATE sessions SET cache_armed_at=unixepoch()-86400*30"
                 " WHERE id=%lld;", (long long)lapsed);
        sqlite3_exec(db, sql, NULL, NULL, NULL);
        db_close(db);
    }
    CHECK(lapsed > 0, "seed the lapsed session");

    mock_server_reset();
    daemon = integ_spawn_daemon(&ENV, exe, LOGD, 0);
    CHECK(daemon > 0, "spawn the daemon");
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=%lld AND type='system'"
             " AND content LIKE 'cache note %%' AND json_extract(data,'$.source')='hook:ce'",
             (long long)sid);
    int64_t notes = 0;
    for (int i = 0; i < 150 && notes == 0; i++) {
        usleep(100 * 1000);
        notes = db_int(sql, NULL);
    }
    if (notes != 1) integ_dump_log(LOGD);
    CHECK(notes == 1, "the daemon did not run the CacheExpiring hook and apply its edit");
    /* Released, disarmed; no C trim; nothing sent to the model. */
    for (int i = 0; i < 50; i++) {
        snprintf(sql, sizeof(sql), "SELECT state='idle' AND cache_armed_at IS NULL"
                 " FROM sessions WHERE id=%lld", (long long)sid);
        if (db_int(sql, NULL) == 1) break;
        usleep(100 * 1000);
    }
    CHECK(db_int(sql, NULL) == 1, "the session is not idle and disarmed after the hook");
    snprintf(sql, sizeof(sql), "SELECT COUNT(*) FROM entries WHERE session_id=%lld"
             " AND type='system' AND json_extract(data,'$.source')='" TRIM_SOURCE "'",
             (long long)sid);
    CHECK(db_int(sql, NULL) == 0, "C trimmed an idle session after CacheExpiring");
    CHECK(db_int("SELECT fail_count FROM hooks WHERE extension_name='ce'", NULL) == 0,
          "the hook was counted as failed");
    snprintf(sql, sizeof(sql), "SELECT cache_armed_at IS NULL AND NOT EXISTS (SELECT 1"
             " FROM entries WHERE session_id=%lld AND type='system') FROM sessions"
             " WHERE id=%lld", (long long)lapsed, (long long)lapsed);
    CHECK(db_int(sql, NULL) == 1, "the long-lapsed session was fired or left armed");
    CHECK(mock_server_request_count() == 0, "the daemon sent a request");
    /* Once: a second later, through more turns of the loop, still one. */
    usleep(1000 * 1000);
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=%lld AND type='system'"
             " AND json_extract(data,'$.source')='hook:ce'", (long long)sid);
    CHECK(db_int(sql, NULL) == 1, "the hook fired again in the same idle period");

    printf("All CacheExpiring integration tests passed.\n");
    rc = 0;
done:
    integ_stop(daemon, SIGTERM);
    mock_server_stop();
    free(agent);
    if (rc == 0) (void)!system("rm -rf " ROOT);
    return rc;
}
