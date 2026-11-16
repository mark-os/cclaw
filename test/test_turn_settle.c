/* The turnSettled event end to end (src/settle.c): real hook scripts, forked
 * through the real --run-tool broker, against a finished turn in a temp DB.
 * What is proven here is the hold — a session enters 'settling', nothing
 * consumes its inbox meanwhile, and every way the script can end returns the
 * session to idle with the right thing done to the turn.
 *
 * The scripts run under the `host` profile so the test does not depend on
 * the machine having unprivileged user namespaces; the sandboxed path is the
 * same code and is exercised on a real instance (specs/hooks.md).
 *
 * main() doubles as the broker: the children re-exec this binary. */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "advance.h"
#include "agent_setup.h"
#include "child.h"
#include "config.h"
#include "config_registry.h"
#include "db.h"
#include "dispatch.h"
#include "extension_manifest.h"
#include "hook.h"
#include "proc.h"
#include "run_tool.h"
#include "settle.h"
#include "sweep.h"
#include "test_util.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* A directory of its own: the extension store is <db dir>/extensions. */
#define ROOT    "/tmp/cclaw_test_turn_settle"
#define DB_PATH ROOT "/cclaw.db"

static sqlite3 *g_db;

static void run(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "FAIL sql: %s\n  %s\n", err ? err : "?", sql);
        abort();
    }
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

/* Promote a one-hook extension owned by (and attached to) `agent`. */
static void install_as(const char *name, const char *agent, int wait, const char *js) {
    char dir[256], path[320], manifest[512];
    snprintf(dir, sizeof(dir), ROOT "/bundle_%s", name);
    mkdir(dir, 0755);
    snprintf(manifest, sizeof(manifest),
             "{\"name\":\"%s\",\"version\":\"1.0.0\","
             "\"config\":[{\"key\":\"greeting\",\"default\":\"hello\",\"description\":\"a setting the script reads\"}],"
             "\"hooks\":[{\"event\":\"turnSettled\",\"handler\":\"h.qjs\"%s}]}",
             name, wait ? "" : ",\"wait\":false");
    snprintf(path, sizeof(path), "%s/extension.json", dir);
    write_file(path, manifest);
    snprintf(path, sizeof(path), "%s/h.qjs", dir);
    write_file(path, js);
    char *err = NULL;
    if (extension_install(g_db, dir, agent, 0, &err) != 0) {
        fprintf(stderr, "FAIL install %s: %s\n", name, err ? err : "?");
        abort();
    }
    free(err);
}

static void install(const char *name, const char *agent, const char *js) {
    install_as(name, agent, 1, js);
}

/* A finished turn on a new top-level session: one call of a droppable tool
 * with a `lines`-line result, then the answer. */
static int64_t seed_turn(const char *agent, int lines) {
    int64_t sid = session_create(g_db, "t", agent, -1, 0);
    assert(sid > 0);
    Message u = {.role = ROLE_USER, .content = "go"};
    assert(entry_append_with_iteration(g_db, sid, &u, 0) > 0);
    int64_t it = db_next_iteration_id(g_db, sid);
    assert(entry_append_typed(g_db, sid, it, "assistant_message", 0, "looking", NULL,
                              NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
    int64_t call = entry_append_typed(g_db, sid, it, "tool_call", 0, "{}", "c1", "look",
                                      0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(call > 0);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
             " VALUES(%lld,%lld,'c1','look','done');", (long long)sid, (long long)call);
    run(sql);
    size_t cap = (size_t)lines * 48 + 1, len = 0;
    char *body = malloc(cap);
    assert(body);
    for (int i = 1; i <= lines; i++)
        len += (size_t)snprintf(body + len, cap - len,
                                "result line %06d ..........................\n", i);
    assert(entry_append_typed(g_db, sid, it, "tool_result", 0, body, "c1", "look", 0,
                              STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
    free(body);
    it = db_next_iteration_id(g_db, sid);
    assert(entry_append_typed(g_db, sid, it, "assistant_message", 0, "done", NULL, NULL,
                              0, STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
    return sid;
}

static int is_state(int64_t sid, const char *want) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db, "SELECT state FROM sessions WHERE id=?1",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, sid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int same = strcmp((const char *)sqlite3_column_text(st, 0), want) == 0;
    sqlite3_finalize(st);
    return same;
}

static int64_t count(const char *sql, int64_t sid) {
    return db_scalar_i64(g_db, sql, sid, -1);
}

/* Run the loop's child handling until the session is idle and no child is
 * left. Bounded: a hook that never ends fails the assertion, not the suite. */
static void pump_until_idle(int64_t sid) {
    for (int i = 0; i < 240; i++) {
        reap_children();
        child_sweep_deadlines();
        if (is_state(sid, "idle") && child_count() == 0) return;
        usleep(50 * 1000);
    }
    fprintf(stderr, "FAIL: session %lld still held after 12s (children=%d)\n",
            (long long)sid, child_count());
    abort();
}

static int fail_count(const char *ext) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db, "SELECT fail_count FROM hooks WHERE extension_name=?1",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, ext, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

#define DROP_C1 "return JSON.stringify({drop:['c1'], note:'dropped the lookup'});"
#define N_CALLS "SELECT COUNT(*) FROM entries e WHERE e.session_id=?1 AND e.type='tool_call'" \
                " AND e.id IN (WITH RECURSIVE b(id) AS (SELECT leaf_id FROM sessions WHERE id=?1" \
                " UNION ALL SELECT e2.parent_id FROM entries e2 JOIN b ON e2.id=b.id) SELECT id FROM b)"
#define N_REVIEWS "SELECT COUNT(*) FROM entries WHERE session_id=?1 AND type='turn_review'"

/* The good path: held, inbox left alone, reply applied, released. Also what
 * the script is handed: the turn as `args`, of any size (this one is far past
 * the 32KB request blob), and its own extension's settings. */
static void test_reviewed(void) {
    install("rv", "Rv",
        "if (args.event !== 'turnSettled' || args.iterations[0].calls[0].call_id !== 'c1')"
        "  throw new Error('unexpected input');"
        "if (args.iterations[0].calls[0].result.lines !== 4000) throw new Error('truncated input');"
        "if (getConfig('greeting') !== 'hello') throw new Error('no config');"
        DROP_C1);
    int64_t sid = seed_turn("Rv", 4000);
    assert(count(N_CALLS, sid) == 1);

    assert(turn_settle_begin(sid, "Rv") == 1);
    assert(is_state(sid, "settling"));
    assert(child_hook_holds(sid));

    /* A message arriving now queues; advancing the session decides nothing. */
    assert(inbox_insert(g_db, sid, "cli", NULL, "are you there") > 0);
    AdvanceOutput out = advance_session(g_db, sid, 0);
    assert(out.action == ADVANCE_WAITING);
    assert(is_state(sid, "settling"));
    assert(count("SELECT COUNT(*) FROM inbox WHERE session_id=?1 AND consumed=0", sid) == 1);
    /* The sweep leaves a hold with a live child alone. */
    turn_settle_sweep();
    assert(is_state(sid, "settling"));

    /* No LLM worker in this process, so the turn the queued message opens
     * cannot be dispatched; that it was opened is the point. */
    for (int i = 0; i < 240 && child_count() > 0; i++) {
        reap_children();
        usleep(50 * 1000);
    }
    assert(child_count() == 0);
    assert(!is_state(sid, "settling"));
    assert(count(N_REVIEWS, sid) == 1);
    assert(count(N_CALLS, sid) == 0);
    assert(count("SELECT COUNT(*) FROM inbox WHERE session_id=?1 AND consumed=0", sid) == 0);
    /* The queued message became the next turn's user entry, after the
     * reviewed turn's answer. */
    assert(count("SELECT type='user_message' AND content='are you there' FROM entries"
                 " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1)", sid) == 1);
    assert(fail_count("rv") == 0);
    printf("  PASS test_reviewed\n");
}

/* Garbage, an exception, and a clean "nothing to do": the turn is untouched
 * each time; the first two count as failures, the third as a success. */
static void test_outcomes(void) {
    install("junk", "Junk", "return 'not json {';");
    install("boom", "Boom", "throw new Error('reviewer blew up');");
    install("nil", "Nil", "return '{}';");
    static const struct { const char *agent, *ext; int fails; } cases[] = {
        { "Junk", "junk", 1 }, { "Boom", "boom", 1 }, { "Nil", "nil", 0 } };
    for (size_t i = 0; i < 3; i++) {
        int64_t sid = seed_turn(cases[i].agent, 400);
        assert(turn_settle_begin(sid, cases[i].agent) == 1);
        assert(is_state(sid, "settling"));
        pump_until_idle(sid);
        assert(count(N_REVIEWS, sid) == 0);
        assert(count(N_CALLS, sid) == 1);
        if (fail_count(cases[i].ext) != cases[i].fails) {
            fprintf(stderr, "FAIL %s: fail_count %d want %d\n", cases[i].ext,
                    fail_count(cases[i].ext), cases[i].fails);
            abort();
        }
    }
    /* Three in a row is what the operator hears about; a run that works in
     * between starts the count over. */
    for (int n = 2; n <= 3; n++) {
        int64_t sid = seed_turn("Junk", 400);
        assert(turn_settle_begin(sid, "Junk") == 1);
        pump_until_idle(sid);
        assert(fail_count("junk") == n);
    }
    printf("  PASS test_outcomes\n");
}

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* A script that outlives its deadline is ended and the turn left untouched —
 * by the broker's own timer at the deadline, and, when the broker itself is
 * stuck, by the loop's sweep a few seconds behind it. The script blocks for
 * real (opening a FIFO nobody writes): a busy loop would trip the engine's
 * instruction limit and prove nothing about the clock. */
static void test_deadline(void) {
    assert(mkfifo(ROOT "/ws/never.fifo", 0600) == 0);
    install("slow", "Slow", "fs.readFile('" ROOT "/ws/never.fifo'); return '{}';");
    assert(config_set(g_db, "turn_settled_timeout", "1") == 0);

    int64_t sid = seed_turn("Slow", 400);
    double t0 = now_s();
    assert(turn_settle_begin(sid, "Slow") == 1);
    assert(is_state(sid, "settling"));
    pump_until_idle(sid);
    double held = now_s() - t0;
    if (held < 0.9 || held > 4.0) {
        fprintf(stderr, "FAIL: held %.2fs, deadline was 1s\n", held);
        abort();
    }
    assert(count(N_REVIEWS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("slow") == 1);

    /* The backstop: freeze the broker so its timer cannot fire. */
    sid = seed_turn("Slow", 400);
    t0 = now_s();
    assert(turn_settle_begin(sid, "Slow") == 1);
    assert(child_count() == 1);
    assert(kill(child_at(0)->pid, SIGSTOP) == 0);
    pump_until_idle(sid);
    held = now_s() - t0;
    if (held < 5.0 || held > 10.0) {
        fprintf(stderr, "FAIL: frozen broker held %.2fs, kill was due at 6s\n", held);
        abort();
    }
    assert(count(N_REVIEWS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("slow") == 2);

    /* Killed from outside (the OOM killer, an operator): reaped as a signal
     * death, a failure like any other, and the hold ends at once. */
    sid = seed_turn("Slow", 400);
    t0 = now_s();
    assert(turn_settle_begin(sid, "Slow") == 1);
    assert(child_count() == 1);
    assert(kill(child_at(0)->pid, SIGKILL) == 0);
    pump_until_idle(sid);
    assert(now_s() - t0 < 0.9);
    assert(count(N_REVIEWS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("slow") == 3);

    assert(config_set(g_db, "turn_settled_timeout", "20") == 0);
    printf("  PASS test_deadline\n");
}

/* Two awaited hooks on one agent run one at a time in extension-name order,
 * each on the turn as the one before left it. The second cannot review a
 * turn that already has a review: that is a refusal it is charged with, not
 * a state the session is stuck in. */
static void test_chain(void) {
    install("ch_a", "Chain", DROP_C1);
    install("ch_b", "Chain",
            "if (args.iterations.length !== 0) throw new Error('saw the unedited turn');"
            "return JSON.stringify({note:'second opinion'});");
    int64_t sid = seed_turn("Chain", 400);
    assert(turn_settle_begin(sid, "Chain") == 1);
    assert(child_count() == 1);                 /* one at a time */
    pump_until_idle(sid);
    assert(count(N_REVIEWS, sid) == 1 && count(N_CALLS, sid) == 0);
    assert(count("SELECT content='dropped the lookup' FROM entries"
                 " WHERE session_id=?1 AND type='turn_review'", sid) == 1);
    assert(fail_count("ch_a") == 0);
    assert(fail_count("ch_b") == 1);            /* rejected, not thrown */
    printf("  PASS test_chain\n");
}

/* wait:false — started, never held for, and its reply goes nowhere. */
static void test_not_awaited(void) {
    install_as("obs", "Obs", 0, DROP_C1);
    assert(count("SELECT wait FROM hooks WHERE extension_name='obs' AND ?1 = 1", 1) == 0);
    int64_t sid = seed_turn("Obs", 400);
    assert(turn_settle_begin(sid, "Obs") == 0);
    assert(is_state(sid, "idle"));
    assert(child_count() == 1 && !child_hook_holds(sid));
    pump_until_idle(sid);
    assert(count(N_REVIEWS, sid) == 0 && count(N_CALLS, sid) == 1);
    assert(fail_count("obs") == 0);
    printf("  PASS test_not_awaited\n");
}

/* Nothing to run, nothing held: a sub-agent's session, an agent without a
 * hook. And a hook whose script is gone starts nothing. */
static void test_not_started(void) {
    int64_t parent = seed_turn("Rv", 2);
    int64_t sub = seed_turn("Rv", 400);
    char sql[128];
    snprintf(sql, sizeof(sql), "UPDATE sessions SET parent_session_id=%lld, depth=1"
             " WHERE id=%lld;", (long long)parent, (long long)sub);
    run(sql);
    assert(turn_settle_begin(sub, "Rv") == 0);
    assert(is_state(sub, "idle") && child_count() == 0);

    db_agent_upsert(g_db, "Plain", NULL, NULL);
    int64_t plain = seed_turn("Plain", 400);
    assert(turn_settle_begin(plain, "Plain") == 0);
    assert(is_state(plain, "idle") && child_count() == 0);

    install("gone", "Gone", DROP_C1);
    assert(unlink(ROOT "/extensions/gone/h.qjs") == 0);
    int64_t sid = seed_turn("Gone", 400);
    assert(turn_settle_begin(sid, "Gone") == 0);
    assert(is_state(sid, "idle") && child_count() == 0);
    assert(fail_count("gone") == 0);            /* not a failure of the script */
    printf("  PASS test_not_started\n");
}

/* The lost-wake backstop: a session this process holds with no hook child
 * behind it is released by the sweep. */
static void test_sweep(void) {
    int64_t sid = seed_turn("Rv", 400);
    assert(session_set_state(g_db, sid, "settling") == 0);
    assert(!child_hook_holds(sid));
    turn_settle_sweep();
    assert(is_state(sid, "idle"));
    assert(count(N_REVIEWS, sid) == 0 && count(N_CALLS, sid) == 1);
    printf("  PASS test_sweep\n");
}

int main(int argc, char *argv[]) {
    if (argc >= 2 && strcmp(argv[1], "--run-tool") == 0)
        return run_tool_main();
    TEST_INIT();
    printf("test_turn_settle:\n");

    (void)system("rm -rf " ROOT);
    mkdir(ROOT, 0755);
    mkdir(ROOT "/ws", 0755);
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    setenv("CCLAW_WORKSPACE", ROOT "/ws", 1);
    setenv("CCLAW_SANDBOX_PROFILE", "host", 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);

    g_db = test_db_open(DB_PATH);
    assert(g_db);
    static const char *const agents[] = { "Rv", "Junk", "Boom", "Nil", "Slow",
                                          "Chain", "Obs", "Gone" };
    for (size_t i = 0; i < sizeof(agents) / sizeof(agents[0]); i++)
        db_agent_upsert(g_db, agents[i], NULL, NULL);
    run("INSERT INTO tools(name, droppable) VALUES('look',1);");
    assert(config_set(g_db, "turn_settled_timeout", "20") == 0);
    /* Keep the scripts' scratch out of the shared /tmp tree. */
    assert(config_set(g_db, "tmp_root", ROOT) == 0);

    Config *cfg = config_load(g_db);
    assert(cfg);
    proc_set_db(g_db);
    proc_set_cfg(cfg);
    static AgentSetup setup;
    assert(agent_setup_init(&setup, g_db, 0, cfg, "Rv") == 0);
    proc_set_tool_setup(&setup);

    test_reviewed();
    test_outcomes();
    test_deadline();
    test_chain();
    test_not_awaited();
    test_not_started();
    test_sweep();

    agent_setup_destroy(&setup);
    config_free(cfg);
    db_close(g_db);
    (void)system("rm -rf " ROOT);
    printf("All turn_settle tests passed.\n");
    return 0;
}
