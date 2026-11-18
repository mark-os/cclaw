/* A long turn against a small context, with a ContextThreshold hook.
 *
 * The real binary runs one `-p` turn of many iterations against a mock LLM,
 * the agent's routing entry given a trim threshold a few iterations above its
 * head, and a ContextThreshold hook attached. At the end of every iteration
 * over the threshold the session is held while the script runs, then C's
 * check trims what the script left over the threshold, then the next request
 * goes out. Three hooks, one turn each:
 *
 *   (a) a script that replies with no edits: it runs (and sees the turn still
 *       running, with the numbers), C trims after it, the turn finishes;
 *   (b) a script that crashes: counted, and the turn still finishes on C's
 *       trim;
 *   (c) a script that outlives its own one-second timeout: the same;
 *   (d) a script that summarises every iteration of the running turn but the
 *       newest: the edit lands between iterations, the session is under the
 *       threshold after it so C never cuts the running turn (it may still
 *       drop an old turn when the script has nothing to summarise yet), the
 *       turn finishes, and the last request carries the summaries and not
 *       what they replaced.
 *
 * Those four run with the bundled `compaction` switched off for the agent
 * (it is attached to the default agent of a fresh DB, which is checked).
 * Then the bundled extension itself, the test's own switched off, each on a
 * session of its own:
 *
 *   (e) its LLM() call answered by the mock: the turn compacts mid-turn
 *       through it — one summary that folds the last, actions listed — C
 *       never cuts it, and the turn finishes;
 *   (f) its LLM() call failing: each run is a counted failure, C's trim keeps
 *       the turn going, it finishes, and the third failure turns the hook off.
 */
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

#define ROOT    "/tmp/cclaw_integ_ctxhook"
#define DB_PATH ROOT "/cclaw.db"
#define WS      ROOT "/ws"
#define BUNDLE  ROOT "/bundle"
#define LOG1    ROOT "/run1.log"
#define LOG2    ROOT "/run2.log"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while (0)

#define STEPS      12
#define STEP_BYTES 6000          /* ~2000 tokens of assistant text per iteration */

static const char *RESP_FINAL =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"all done\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

/* Iteration i: STEP_BYTES of text tagged MARK<i>, and a call to a tool that
 * does not exist (answered inline with an error — no grant, no fork). */
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
        setenv("CCLAW_WORKSPACE", WS, 1);
        setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
        setenv("OPENROUTER_API_KEY", "test-key", 1);
        setenv("CCLAW_MODEL", "test-model", 1);
        setenv("CCLAW_MAX_ITERATIONS", "40", 1);
        /* The hook scripts run unsandboxed so the test does not depend on
         * the machine having user namespaces (test_turn_settle does the
         * same); the sandboxed path is the same code. */
        setenv("CCLAW_SANDBOX_PROFILE", "host", 1);
        if (sid > 0) {
            char sbuf[32];
            snprintf(sbuf, sizeof(sbuf), "%lld", (long long)sid);
            execl(exe, exe, "-v", "-p", prompt, "-s", sbuf, (char *)NULL);
        } else {
            execl(exe, exe, "-p", prompt, (char *)NULL);
        }
        _exit(111);
    }
    for (int i = 0; i < 350; i++) {   /* 35s deadline */
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

/* The agent's head as the trim estimates it, a threshold four iterations
 * above it and a target two above; and the operator's notes go to an agent
 * with no session, so a hook disabled by its failures opens no turn here. */
static int set_limits(int64_t sid, char *agent_out, size_t cap) {
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
    /* The bundled compaction is the default agent's from its first run;
     * these cases test their own hook, so it is switched off — rolling
     * (decision 5) is exactly this. */
    snprintf(sql, sizeof(sql),
             "UPDATE agent_extensions SET enabled=0 WHERE agent_name='%s'"
             " AND extension_name='compaction' AND enabled=1;", agent ? agent : "");
    if (rc == 0 && (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK ||
                    sqlite3_changes(db) != 1)) {
        fprintf(stderr, "the default agent does not have the bundled compaction\n");
        rc = -1;
    }
    test_seed_agent(db, "Nobody");
    if (rc == 0) rc = config_set(db, "default_agent", "Nobody");
    if (rc == 0) rc = config_set(db, "hook_timeout", "20");
    snprintf(agent_out, cap, "%s", agent ? agent : "");
    free(agent);
    sqlite3_close(db);
    return rc;
}

/* Promote (or re-promote) the one-hook extension for `agent`. */
static int install_hook(const char *agent, const char *fields, const char *js) {
    char manifest[512];
    snprintf(manifest, sizeof(manifest),
             "{\"name\":\"compact\",\"version\":\"1.0.0\",\"hooks\":["
             "{\"event\":\"ContextThreshold\",\"handler\":\"h.qjs\"%s}]}", fields);
    FILE *f = fopen(BUNDLE "/extension.json", "w");
    if (!f) return -1;
    fputs(manifest, f);
    fclose(f);
    f = fopen(BUNDLE "/h.qjs", "w");
    if (!f) return -1;
    fputs(js, f);
    fclose(f);
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return -1;
    char *err = NULL;
    int rc = extension_install(db, BUNDLE, agent, 0, &err);
    if (rc != 0) fprintf(stderr, "install: %s\n", err ? err : "?");
    free(err);
    sqlite3_close(db);
    return rc;
}

static int count_in_log(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof(line), f)) if (strstr(line, needle)) n++;
    fclose(f);
    return n;
}

/* One long turn on `sid`. It must end on the model's answer, after every
 * step was asked for, having trimmed mid-turn more than once. *first_id is
 * where to start counting this turn's rows. */
/* The bundled script's request: its instructions open with this. */
#define SUMMARISER "You compress the oldest part of an AI agent"
static int g_summariser_down;   /* (f): the bundled script's model answers 500 */

static int long_turn(const char *exe, const char *url, int64_t sid, const char *label) {
    char sql[512];
    int64_t since = db_count("SELECT COALESCE(MAX(id),0) FROM entries;");
    mock_server_reset();
    if (g_summariser_down) mock_server_answer_when(SUMMARISER, 500, "{\"error\":\"down\"}");
    for (int i = 0; i < STEPS; i++) {
        char *b = step_response(i);
        mock_server_enqueue(200, b);
        free(b);
    }
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "work through it", sid, LOG2) != 0) {
        fprintf(stderr, "FAIL %s: the run did not exit\n", label);
        dump_log(LOG2);
        return -1;
    }
    if (mock_server_request_count() != STEPS + 1) {
        fprintf(stderr, "FAIL %s: %d requests, expected %d\n", label,
                mock_server_request_count(), STEPS + 1);
        dump_log(LOG2);
        return -1;
    }
    snprintf(sql, sizeof(sql),
             "SELECT content='all done' AND stop_reason=1 FROM entries"
             " WHERE id=(SELECT leaf_id FROM sessions WHERE id=%lld);", (long long)sid);
    if (db_count(sql) != 1) {
        fprintf(stderr, "FAIL %s: the turn did not end on the model's answer\n", label);
        dump_log(LOG2);
        return -1;
    }
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=%lld AND id > %lld"
             " AND type='system' AND json_extract(data,'$.source')='" TRIM_SOURCE "'"
             " AND json_extract(data,'$.turn_edits[0].dropped.iterations') > 0;",
             (long long)sid, (long long)since);
    if (db_count(sql) < 2) {
        fprintf(stderr, "FAIL %s: expected at least two mid-turn trims, got %lld\n", label,
                (long long)db_count(sql));
        dump_log(LOG2);
        return -1;
    }
    const char *last = mock_server_last_request_body();
    if (!last || strstr(last, "MARK00") || !strstr(last, "MARK11")) {
        fprintf(stderr, "FAIL %s: the last request is not the trimmed branch\n", label);
        return -1;
    }
    return 0;
}

/* (d): one long turn where the script, not C, keeps the session small. */
static int summarise_turn(const char *exe, const char *url, int64_t sid) {
    char sql[512];
    int64_t since = db_count("SELECT COALESCE(MAX(id),0) FROM entries;");
    mock_server_reset();
    for (int i = 0; i < STEPS; i++) {
        char *b = step_response(i);
        mock_server_enqueue(200, b);
        free(b);
    }
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "work through it", sid, LOG2) != 0 ||
        mock_server_request_count() != STEPS + 1) {
        fprintf(stderr, "FAIL (d): the run did not finish (%d requests)\n",
                mock_server_request_count());
        dump_log(LOG2);
        return -1;
    }
    snprintf(sql, sizeof(sql),
             "SELECT content='all done' AND stop_reason=1 FROM entries"
             " WHERE id=(SELECT leaf_id FROM sessions WHERE id=%lld);", (long long)sid);
    if (db_count(sql) != 1) {
        fprintf(stderr, "FAIL (d): the turn did not end on the model's answer\n");
        dump_log(LOG2);
        return -1;
    }
    int applied = count_in_log(LOG2, "hook ContextThreshold ext=compact outcome=applied");
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=%lld AND id > %lld"
             " AND type='compaction' AND json_extract(data,'$.source')='hook:compact'"
             " AND turn_id=(SELECT turn_id FROM entries WHERE id=(SELECT leaf_id FROM"
             " sessions WHERE id=%lld));", (long long)sid, (long long)since, (long long)sid);
    int64_t summaries = db_count(sql);
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=%lld AND id > %lld"
             " AND type='system' AND json_extract(data,'$.source')='" TRIM_SOURCE "'"
             " AND json_extract(data,'$.turn_edits[0].dropped.iterations') > 0;",
             (long long)sid, (long long)since);
    int64_t trims = db_count(sql);
    if (applied < 2 || summaries != applied || trims != 0) {
        fprintf(stderr, "FAIL (d): %d applied, %lld summaries in the turn, %lld C trims"
                " of its iterations\n",
                applied, (long long)summaries, (long long)trims);
        dump_log(LOG2);
        return -1;
    }
    const char *last = mock_server_last_request_body();
    if (!last || !strstr(last, "SUMMARY of iterations") || strstr(last, "MARK00") ||
        strstr(last, "MARK01") || !strstr(last, "MARK11")) {
        fprintf(stderr, "FAIL (d): the last request is not the summarised branch\n");
        return -1;
    }
    if (db_count("SELECT fail_count FROM hooks WHERE extension_name='compact'") != 0) {
        fprintf(stderr, "FAIL (d): the hook's applied replies were counted as failures\n");
        return -1;
    }
    return 0;
}

/* Which compaction the agent runs: the bundled one, or the test's own. */
static int use_bundled(const char *agent, int bundled) {
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return -1;
    char sql[512];
    snprintf(sql, sizeof(sql),
             "UPDATE agent_extensions SET enabled = (extension_name='compaction') = %d"
             " WHERE agent_name='%s' AND extension_name IN ('compaction','compact');",
             bundled, agent);
    int rc = sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK &&
             sqlite3_changes(db) == 2 ? 0 : -1;
    sqlite3_close(db);
    return rc;
}

/* A session of its own for the agent, so the turn is the whole session. */
static int64_t new_session(const char *agent) {
    sqlite3 *db = db_open(DB_PATH);
    if (!db) return -1;
    int64_t sid = session_create(db, "bundled", agent, -1, 0);
    sqlite3_close(db);
    return sid;
}

static const char *RESP_SUMMARY =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\","
    "\"content\":\"BUNDLED SUMMARY: each step looked and found nothing.\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":9}}";

/* (e): the bundled extension keeps the running turn small. */
static int bundled_turn(const char *exe, const char *url, int64_t sid) {
    char sql[512];
    mock_server_reset();
    mock_server_answer_when(SUMMARISER, 200, RESP_SUMMARY);
    for (int i = 0; i < STEPS; i++) {
        char *b = step_response(i);
        mock_server_enqueue(200, b);
        free(b);
    }
    mock_server_enqueue(200, RESP_FINAL);
    if (run_cli_p(exe, url, "work through it", sid, LOG2) != 0 ||
        mock_server_request_count() != STEPS + 1) {
        fprintf(stderr, "FAIL (e): the run did not finish (%d requests)\n",
                mock_server_request_count());
        dump_log(LOG2);
        return -1;
    }
    snprintf(sql, sizeof(sql),
             "SELECT content='all done' AND stop_reason=1 FROM entries"
             " WHERE id=(SELECT leaf_id FROM sessions WHERE id=%lld);", (long long)sid);
    if (db_count(sql) != 1) {
        fprintf(stderr, "FAIL (e): the turn did not end on the model's answer\n");
        dump_log(LOG2);
        return -1;
    }
    int applied = count_in_log(LOG2, "hook ContextThreshold ext=compaction outcome=applied");
    int asked = mock_server_rule_hits();
    snprintf(sql, sizeof(sql),
             "SELECT COUNT(*) FROM entries WHERE session_id=%lld AND type='system'"
             " AND json_extract(data,'$.source')='" TRIM_SOURCE "';", (long long)sid);
    int64_t trims = db_count(sql);
    /* On the branch: one summary, the bundled one, listing the actions. */
    snprintf(sql, sizeof(sql),
             "WITH RECURSIVE b(id) AS (SELECT leaf_id FROM sessions WHERE id=%lld"
             " UNION ALL SELECT e.parent_id FROM entries e JOIN b ON e.id=b.id)"
             " SELECT COUNT(*) FROM entries e JOIN b ON b.id=e.id WHERE e.type='compaction'"
             " AND json_extract(e.data,'$.source')='hook:compaction'"
             " AND e.content LIKE 'BUNDLED SUMMARY%%Actions already taken%%no_such_tool%%';",
             (long long)sid);
    int64_t on_branch = db_count(sql);
    if (applied < 2 || asked != applied || trims != 0 || on_branch != 1) {
        fprintf(stderr, "FAIL (e): %d applied, %d summariser calls, %lld C trims,"
                " %lld bundled summaries on the branch\n",
                applied, asked, (long long)trims, (long long)on_branch);
        dump_log(LOG2);
        return -1;
    }
    const char *last = mock_server_last_request_body();
    if (!last || !strstr(last, "BUNDLED SUMMARY") || strstr(last, "MARK00") ||
        !strstr(last, "MARK11") || !strstr(last, "work through it")) {
        fprintf(stderr, "FAIL (e): the last request is not the summarised branch\n");
        return -1;
    }
    if (db_count("SELECT fail_count FROM hooks WHERE extension_name='compaction'") != 0) {
        fprintf(stderr, "FAIL (e): the bundled hook's runs were counted as failures\n");
        return -1;
    }
    return 0;
}

static void cleanup_files(void) {
    (void)system("rm -rf " ROOT);
}

int main(void) {
    TEST_INIT();
    alarm(42);   /* below make's 45s wrapper: fail loud, not hung */
    cleanup_files();
    mkdir(ROOT, 0755);
    mkdir(WS, 0755);
    mkdir(BUNDLE, 0755);
    if (mkfifo(WS "/never.fifo", 0600) != 0) FAIL("mkfifo");

    char exe[PATH_MAX];
    if (!realpath("build/cclaw", exe)) FAIL("realpath build/cclaw (run from repo root)");
    int port = mock_server_start();
    if (port < 0) FAIL("mock_server_start");
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);

    int rc = 1;
    char agent[64] = "";
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
    if (set_limits(sid, agent, sizeof(agent)) != 0) { fprintf(stderr, "FAIL: limits\n"); goto done; }

    /* (a) A script that looks and leaves it to C: a reply with no edits.
     * It throws (and so fails, and is counted) unless it is handed the turn
     * still running and the numbers it decides on. */
    if (install_hook(agent, "",
            "var c = args.context;"
            "if (args.event !== 'ContextThreshold' || args.final !== null"
            "    || !(c.tokens > c.trim_threshold) || !(c.prune_target < c.trim_threshold)"
            "    || args.iterations.length < 1)"
            "  throw new Error('unexpected input');"
            "return '{}';") != 0) { fprintf(stderr, "FAIL: install (a)\n"); goto done; }
    if (long_turn(exe, url, sid, "(a) no edits") != 0) goto done;
    int ran = count_in_log(LOG2, "hook ContextThreshold ext=compact outcome=nothing");
    if (ran < 2 || db_count("SELECT fail_count FROM hooks WHERE extension_name='compact'") != 0) {
        fprintf(stderr, "FAIL (a): the hook ran %d times, fail_count %lld\n", ran,
                (long long)db_count("SELECT fail_count FROM hooks WHERE extension_name='compact'"));
        dump_log(LOG2);
        goto done;
    }
    /* Each run is followed by C's check: the log has a trim after a hook. */
    if (count_in_log(LOG2, "trim session=") < 2) {
        fprintf(stderr, "FAIL (a): no trim logged after the hook\n");
        dump_log(LOG2);
        goto done;
    }
    printf("  PASS long_turn_hook_no_edits_then_trim\n");

    /* (b) A script that crashes: nothing it did, counted, C's trim. */
    if (install_hook(agent, "", "throw new Error('compactor died');") != 0) {
        fprintf(stderr, "FAIL: install (b)\n");
        goto done;
    }
    if (long_turn(exe, url, sid, "(b) crash") != 0) goto done;
    if (count_in_log(LOG2, "hook ContextThreshold ext=compact outcome=failed") < 1 ||
        db_count("SELECT fail_count FROM hooks WHERE extension_name='compact'") < 1) {
        fprintf(stderr, "FAIL (b): no counted failure\n");
        dump_log(LOG2);
        goto done;
    }
    printf("  PASS long_turn_hook_crashes_turn_finishes\n");

    /* (c) A script that outlives its own one-second timeout. */
    if (install_hook(agent, ",\"timeout\":1",
                     "fs.readFile('" WS "/never.fifo'); return '{}';") != 0) {
        fprintf(stderr, "FAIL: install (c)\n");
        goto done;
    }
    if (long_turn(exe, url, sid, "(c) timeout") != 0) goto done;
    if (count_in_log(LOG2, "timed out") < 1 ||
        db_count("SELECT fail_count FROM hooks WHERE extension_name='compact'") < 1) {
        fprintf(stderr, "FAIL (c): no counted timeout\n");
        dump_log(LOG2);
        goto done;
    }
    printf("  PASS long_turn_hook_times_out_turn_finishes\n");

    /* (d) The script keeps the session under the threshold itself. */
    if (install_hook(agent, "",
            "var it = args.iterations;"
            "if (args.event !== 'ContextThreshold' || args.final !== null || it.length < 2)"
            "  return '{}';"
            "var ids = it.slice(0, -1).map(function (x) { return x.iteration_id; });"
            "return JSON.stringify({edits: [{op: 'summarise',"
            "  from_iteration: ids[0], to_iteration: ids[ids.length - 1],"
            "  text: 'SUMMARY of iterations ' + ids.join(',') + ': each looked, none found.'}]});")
        != 0) {
        fprintf(stderr, "FAIL: install (d)\n");
        goto done;
    }
    if (summarise_turn(exe, url, sid) != 0) goto done;
    printf("  PASS long_turn_hook_summarises_running_turn\n");

    /* (e) The bundled extension, as shipped. */
    if (use_bundled(agent, 1) != 0) { fprintf(stderr, "FAIL: switch to bundled\n"); goto done; }
    int64_t sid_e = new_session(agent);
    if (sid_e <= 0 || bundled_turn(exe, url, sid_e) != 0) goto done;
    printf("  PASS long_turn_bundled_compaction_summarises_mid_turn\n");

    /* (f) The bundled extension with its model down. */
    int64_t sid_f = new_session(agent);
    if (sid_f <= 0) goto done;
    g_summariser_down = 1;
    if (long_turn(exe, url, sid_f, "(f) bundled, model down") != 0) goto done;
    int failed = count_in_log(LOG2, "hook ContextThreshold ext=compaction outcome=failed");
    if (failed < 1 || mock_server_rule_hits() != failed ||
        db_count("SELECT (disabled=1) = (fail_count>=3) FROM hooks"
                 " WHERE extension_name='compaction' AND event='ContextThreshold'") != 1 ||
        (failed >= 3 && db_count("SELECT disabled FROM hooks WHERE extension_name='compaction'"
                                 " AND event='ContextThreshold'") != 1) ||
        /* Its CacheExpiring hook is a hook of its own: never run here. */
        db_count("SELECT disabled = 0 AND fail_count = 0 FROM hooks"
                 " WHERE extension_name='compaction' AND event='CacheExpiring'") != 1) {
        fprintf(stderr, "FAIL (f): %d failed runs, %d summariser calls\n", failed,
                mock_server_rule_hits());
        dump_log(LOG2);
        goto done;
    }
    printf("  PASS long_turn_bundled_compaction_fails_turn_finishes\n");
    printf("All context-hook integration tests passed.\n");
    rc = 0;

done:
    mock_server_stop();
    if (rc == 0 && !getenv("KEEP")) cleanup_files();
    return rc;
}
