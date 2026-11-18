/* Script hooks, the database side (src/hook.c) and the 'settling' state's
 * guards: which events exist, which hooks an agent runs and in what order,
 * when a finished turn gets its Stop or SubagentStop hooks, the failure count
 * and the flag it sets, what may move a session into and out of the hold,
 * and the hold as a held resource. No forks here — test_turn_settle runs the
 * scripts. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "config_registry.h"
#include "hook.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DB_PATH = "/tmp/test_cclaw_hook.db";
#define EV HOOK_EVENT_STOP

static void run(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "FAIL sql: %s\n  %s\n", err ? err : "?", sql);
        abort();
    }
}

/* Two agents. ext_a: someone else's, published. ext_b: Main's own,
 * unpublished, two handlers. ext_c: someone else's, NOT published. */
static sqlite3 *open_seeded(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    test_seed_agent(db, "Main");
    test_seed_agent(db, "Other");
    test_seed_agent(db, "Bare");
    run(db,
        "INSERT INTO extensions(name, path, owner_agent, published) VALUES"
        " ('ext_a','/s/ext_a','Other',1),('ext_b','/s/ext_b','Main',0),"
        " ('ext_c','/s/ext_c','Other',0);"
        "INSERT INTO hooks(extension_name, event, path, async) VALUES"
        " ('ext_b','Stop','/s/ext_b/z.qjs',0),"
        " ('ext_b','Stop','/s/ext_b/a.qjs',1),"
        " ('ext_a','Stop','/s/ext_a/r.qjs',0),"
        " ('ext_c','Stop','/s/ext_c/r.qjs',0);"
        "INSERT INTO agent_extensions(agent_name, extension_name) VALUES"
        " ('Main','ext_a'),('Main','ext_b'),('Main','ext_c'),"
        " ('Other','ext_c');");
    return db;
}

/* The agent's hooks for the event, in the order hook_next walks them. */
static void expect_chain(sqlite3 *db, const char *agent, int wait, const char *want) {
    char got[512] = "";
    HookRef h;
    const HookRef *after = NULL;
    int rc;
    while ((rc = hook_next(db, agent, EV, wait, after, &h)) == 1) {
        size_t n = strlen(got);
        snprintf(got + n, sizeof(got) - n, "%s%s:%s:%d", n ? " " : "",
                 h.extension, strrchr(h.path, '/') + 1, h.wait);
        after = &h;
    }
    assert(rc == 0);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL chain agent=%s wait=%d\n  got  %s\n  want %s\n",
                agent, wait, got, want);
        abort();
    }
}

static void test_resolution(void) {
    sqlite3 *db = open_seeded();

    /* Extension name, then path; the unpublished foreign extension is not
     * Main's to run even though it is attached. */
    expect_chain(db, "Main", -1, "ext_a:r.qjs:1 ext_b:a.qjs:0 ext_b:z.qjs:1");
    expect_chain(db, "Main", 1, "ext_a:r.qjs:1 ext_b:z.qjs:1");
    expect_chain(db, "Main", 0, "ext_b:a.qjs:0");
    /* Each agent runs its own: Other owns ext_c and has only that attached. */
    expect_chain(db, "Other", -1, "ext_c:r.qjs:1");
    expect_chain(db, "Bare", -1, "");
    /* Another event's rows are not this event's. */
    HookRef h;
    assert(hook_next(db, "Main", HOOK_EVENT_CONTEXT_THRESHOLD, -1, NULL, &h) == 0);
    assert(hook_any(db, "Main", EV) && !hook_any(db, "Main", HOOK_EVENT_SUBAGENT_STOP));
    assert(hook_next(db, "Main", EV, 1, NULL, &h) == 1 && strcmp(h.event, EV) == 0);

    /* Within an extension, the manifest's order (pos) comes before the
     * path; the cursor walks it one hook at a time. */
    run(db, "UPDATE hooks SET pos=1 WHERE path='/s/ext_b/a.qjs';"
            "UPDATE hooks SET pos=0 WHERE path='/s/ext_b/z.qjs';");
    expect_chain(db, "Main", -1, "ext_a:r.qjs:1 ext_b:z.qjs:1 ext_b:a.qjs:0");
    run(db, "UPDATE hooks SET pos=0;");
    /* A hook's own timeout rides along; none is the configured default. */
    run(db, "UPDATE hooks SET timeout=7 WHERE extension_name='ext_a';");
    assert(hook_next(db, "Main", EV, 1, NULL, &h) == 1 && h.timeout == 7);
    run(db, "UPDATE hooks SET timeout=NULL;");
    assert(hook_next(db, "Main", EV, 1, NULL, &h) == 1 && h.timeout == 0);
    /* A hook disabled by its failures is passed over like a switched-off one. */
    run(db, "UPDATE hooks SET disabled=1 WHERE extension_name='ext_a';");
    expect_chain(db, "Main", 1, "ext_b:z.qjs:1");
    run(db, "UPDATE hooks SET disabled=0;");

    /* Read live: attach, and the next call sees it — no restart, no cache. */
    run(db, "INSERT INTO agent_extensions(agent_name, extension_name)"
            " VALUES('Bare','ext_a');");
    expect_chain(db, "Bare", -1, "ext_a:r.qjs:1");
    /* ...and each of the three switches takes it away again. */
    run(db, "UPDATE agent_extensions SET enabled=0 WHERE agent_name='Bare';");
    expect_chain(db, "Bare", -1, "");
    run(db, "UPDATE agent_extensions SET enabled=1 WHERE agent_name='Bare';"
            "UPDATE hooks SET enabled=0 WHERE extension_name='ext_a';");
    expect_chain(db, "Bare", -1, "");
    expect_chain(db, "Main", 1, "ext_b:z.qjs:1");
    run(db, "UPDATE hooks SET enabled=1; UPDATE extensions SET enabled=0 WHERE name='ext_a';");
    expect_chain(db, "Bare", -1, "");
    run(db, "UPDATE extensions SET enabled=1;");
    /* Publishing is what makes someone else's extension runnable. */
    run(db, "UPDATE extensions SET published=1 WHERE name='ext_c';");
    expect_chain(db, "Main", 1, "ext_a:r.qjs:1 ext_b:z.qjs:1 ext_c:r.qjs:1");

    db_close(db);
    printf("  PASS test_resolution\n");
}

/* A finished turn on a new session of `agent`: one tool call whose result is
 * `result_bytes` long, then the answer (or an error stop). */
static int64_t seed_turn(sqlite3 *db, const char *agent, int64_t parent,
                         int result_bytes, StopReason stop) {
    int64_t sid = session_create(db, "t", agent, parent, parent > 0 ? 1 : 0);
    assert(sid > 0);
    Message u = {.role = ROLE_USER, .content = "go"};
    assert(entry_append_with_iteration(db, sid, &u, 0) > 0);
    int64_t it = db_next_iteration_id(db, sid);
    assert(entry_append_typed(db, sid, it, "assistant_message", 0, "looking", NULL,
                              NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
    int64_t call = entry_append_typed(db, sid, it, "tool_call", 0, "{}", "c1", "look",
                                      0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(call > 0);
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
             " VALUES(%lld,%lld,'c1','look','done');", (long long)sid, (long long)call);
    run(db, sql);
    char *body = malloc((size_t)result_bytes + 1);
    assert(body);
    memset(body, 'x', (size_t)result_bytes);
    body[result_bytes] = '\0';
    assert(entry_append_typed(db, sid, it, "tool_result", 0, body, "c1", "look", 0,
                              STOP_REASON_NONE, NULL, 0, 0, 0) > 0);
    free(body);
    it = db_next_iteration_id(db, sid);
    assert(entry_append_typed(db, sid, it, "assistant_message", 0, "done", NULL, NULL,
                              0, stop, NULL, 0, 0, 0) > 0);
    return sid;
}

static void expect_due(sqlite3 *db, int64_t sid, const char *agent, int want,
                       const char *why_has) {
    char why[96];
    int got = hook_stop_due(db, sid, agent, why, sizeof(why));
    if (got != want || (why_has && !strstr(why, why_has))) {
        fprintf(stderr, "FAIL due session=%lld: got %d (%s), want %d (%s)\n",
                (long long)sid, got, why, want, why_has ? why_has : "");
        abort();
    }
}

static void test_eligibility(void) {
    sqlite3 *db = open_seeded();
    /* The deadline is the one setting; a turn's size is the script's call. */
    assert(config_default_int("hook_timeout") == 60);

    int64_t top = seed_turn(db, "Main", -1, 30000, STOP_REASON_NONE);
    expect_due(db, top, "Main", 1, NULL);
    /* A small turn is handed over too: nothing in C judges its worth. */
    int64_t small = seed_turn(db, "Main", -1, 4, STOP_REASON_NONE);
    expect_due(db, small, "Main", 1, NULL);
    /* A sub-agent's session fires SubagentStop, not Stop: Main has only
     * Stop hooks, so its sub-agent's turn has none... */
    int64_t sub = seed_turn(db, "Main", top, 30000, STOP_REASON_NONE);
    assert(strcmp(hook_stop_event(db, sub), HOOK_EVENT_SUBAGENT_STOP) == 0);
    assert(strcmp(hook_stop_event(db, top), HOOK_EVENT_STOP) == 0);
    expect_due(db, sub, "Main", 0, "no hook");
    /* ...until it has a SubagentStop one; and that one is not a Stop hook. */
    run(db, "INSERT INTO hooks(extension_name, event, path) VALUES"
            " ('ext_a','SubagentStop','/s/ext_a/sub.qjs');");
    expect_due(db, sub, "Main", 1, NULL);
    run(db, "UPDATE hooks SET enabled=0 WHERE event='Stop';");
    expect_due(db, top, "Main", 0, "no hook");
    expect_due(db, sub, "Main", 1, NULL);
    run(db, "UPDATE hooks SET enabled=1; DELETE FROM hooks WHERE event='SubagentStop';");
    /* A turn that stopped on an error (provider failure, cutoff). */
    int64_t err = seed_turn(db, "Main", -1, 30000, STOP_REASON_ERROR);
    expect_due(db, err, "Main", 0, "did not end");
    /* A turn still waiting on its answer: the leaf is a tool result. */
    int64_t mid = seed_turn(db, "Main", -1, 30000, STOP_REASON_NONE);
    run(db, "UPDATE sessions SET leaf_id=(SELECT parent_id FROM entries WHERE id=leaf_id)"
            " WHERE id=(SELECT MAX(id) FROM sessions);");
    expect_due(db, mid, "Main", 0, "did not end");
    /* An agent with no hook pays one lookup and nothing else. */
    int64_t bare = seed_turn(db, "Bare", -1, 30000, STOP_REASON_NONE);
    expect_due(db, bare, "Bare", 0, "no hook");

    db_close(db);
    printf("  PASS test_eligibility\n");
}

static int fail_count(sqlite3 *db, const char *path) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT fail_count FROM hooks WHERE path=?1",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, path, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void test_failure_count(void) {
    sqlite3 *db = open_seeded();
    HookRef h, other;
    assert(hook_next(db, "Main", EV, 1, NULL, &h) == 1);           /* ext_a */
    assert(hook_next(db, "Main", EV, 1, &h, &other) == 1);         /* ext_b z */

    #define DISABLED(path) db_scalar_i64(db, "SELECT disabled FROM hooks WHERE path='" \
                                         path "' AND ?1", 1, -1)
    /* A run that works starts the streak over: fail, work, fail, fail is
     * two in a row, not four. */
    assert(hook_record_outcome(db, &h, 1) == 0);
    assert(hook_record_outcome(db, &h, 0) == 0);
    assert(fail_count(db, h.path) == 0);
    assert(hook_record_outcome(db, &h, 1) == 0);
    assert(hook_record_outcome(db, &h, 1) == 0);
    assert(fail_count(db, h.path) == 2 && DISABLED("/s/ext_a/r.qjs") == 0);
    /* The third failure in a row disables the hook, and says so once. */
    assert(hook_record_outcome(db, &h, 1) == 1);
    assert(DISABLED("/s/ext_a/r.qjs") == 1);
    assert(hook_record_outcome(db, &h, 1) == 0);    /* a run already under way */
    assert(fail_count(db, h.path) == 4);
    HookRef next;
    assert(hook_next(db, "Main", EV, 1, NULL, &next) == 1 &&
           strcmp(next.path, other.path) == 0);
    /* Counted per hook, not per session or per agent. */
    assert(fail_count(db, other.path) == 0);
    /* The operator's way back: clear the flag. The next failure trips it
     * again at once (the count is still past the limit) and says so again;
     * a success in between would have started the count over. */
    run(db, "UPDATE hooks SET disabled=0 WHERE path='/s/ext_a/r.qjs';");
    assert(hook_next(db, "Main", EV, 1, NULL, &next) == 1 &&
           strcmp(next.path, h.path) == 0);
    assert(hook_record_outcome(db, &h, 1) == 1);
    run(db, "UPDATE hooks SET disabled=0, fail_count=0 WHERE path='/s/ext_a/r.qjs';");
    assert(hook_record_outcome(db, &h, 1) == 0);
    assert(hook_record_outcome(db, &h, 1) == 0);
    assert(hook_record_outcome(db, &h, 1) == 1);
    #undef DISABLED

    db_close(db);
    printf("  PASS test_failure_count\n");
}

static void expect_state(sqlite3 *db, int64_t sid, const char *want) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?1",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, sid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *got = (const char *)sqlite3_column_text(st, 0);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL state: got %s want %s\n", got, want);
        abort();
    }
    sqlite3_finalize(st);
}

/* 'settling' is entered from idle (turn end) or tool_running (between
 * iterations) and left to idle or tool_running, and nothing else. */
static void test_state_guard(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "g", "Main", -1, 0);

    assert(session_set_state(db, sid, "settling") == 0);
    expect_state(db, sid, "settling");
    /* Nothing that starts or resumes a request can take a held session
     * (tool_running is how a hold between iterations ends, below). */
    static const char *const busy[] = { "llm_running", "rate_limited",
                                        "awaiting_approval", "settling" };
    for (size_t i = 0; i < sizeof(busy) / sizeof(busy[0]); i++) {
        assert(session_set_state(db, sid, busy[i]) != 0);
        expect_state(db, sid, "settling");
    }
    assert(session_set_state(db, sid, "idle") == 0);
    expect_state(db, sid, "idle");

    /* Between iterations: from tool_running into the hold and back. */
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "tool_running") == 0);
    assert(session_set_state(db, sid, "settling") == 0);
    expect_state(db, sid, "settling");
    assert(session_set_state(db, sid, "llm_running") != 0);
    assert(session_set_state(db, sid, "tool_running") == 0);
    expect_state(db, sid, "tool_running");

    /* Any other busy state cannot be put into the hold. */
    static const char *const no_hold[] = { "llm_running", "rate_limited",
                                           "awaiting_approval" };
    for (size_t i = 0; i < sizeof(no_hold) / sizeof(no_hold[0]); i++) {
        run(db, "UPDATE sessions SET state='idle';");
        assert(session_set_state(db, sid, "llm_running") == 0);
        if (strcmp(no_hold[i], "llm_running") != 0)
            assert(session_set_state(db, sid, no_hold[i]) == 0);
        assert(session_set_state(db, sid, "settling") != 0);
    }
    db_close(db);
    printf("  PASS test_state_guard\n");
}

/* A hold whose owner died (daemon killed mid-hold): recovery returns the
 * session to idle and leaves the turn and the inbox exactly as they were —
 * no resume nudge, there was no turn in flight. */
static void test_recovery(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = seed_turn(db, "Main", -1, 30000, STOP_REASON_NONE);
    int64_t leaf = db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", sid, -1);
    int64_t entries = db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE session_id=?",
                                    sid, -1);
    /* Held by an instance that is not in `processes`. */
    db_set_instance_id("dead-instance");
    assert(session_set_state(db, sid, "settling") == 0);
    db_set_instance_id(NULL);
    assert(inbox_insert(db, sid, "cli", NULL, "queued during the hold") > 0);

    assert(db_recover_stale_sessions(db) == 0);
    expect_state(db, sid, "idle");
    assert(db_scalar_i64(db, "SELECT owner_instance IS NULL FROM sessions WHERE id=?",
                         sid, -1) == 1);
    assert(db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", sid, -1) == leaf);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE session_id=?", sid, -1)
           == entries);
    /* The message that queued is still there, and it is the only row. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM inbox WHERE session_id=?"
                             " AND consumed=0", sid, -1) == 1);

    db_close(db);
    printf("  PASS test_recovery\n");
}

/* A session held for a hook holds a resource for session_max_concurrent,
 * like one with a worker job or a running tool child; the session asking
 * is never counted. */
static void test_resource_holder(void) {
    sqlite3 *db = open_seeded();
    int64_t a = session_create(db, "a", "Main", -1, 0);
    int64_t b = session_create(db, "b", "Main", -1, 0);
    assert(session_count_resource_holders(db, b) == 0);
    assert(session_set_state(db, a, "settling") == 0);
    assert(session_count_resource_holders(db, b) == 1);
    assert(session_count_resource_holders(db, a) == 0);
    assert(session_set_state(db, a, "idle") == 0);
    assert(session_count_resource_holders(db, b) == 0);
    db_close(db);
    printf("  PASS test_resource_holder\n");
}

/* The events a manifest may name: the four this build fires; the ones it
 * knows of but does not fire yet are refused by name; anything else is no
 * event at all. */
static void test_events(void) {
    assert(hook_event_refusal("Stop") == NULL);
    assert(hook_event_refusal("SubagentStop") == NULL);
    assert(hook_event_refusal("ContextThreshold") == NULL);
    assert(hook_event_refusal("CacheExpiring") == NULL);
    assert(strstr(hook_event_refusal("PreToolUse"), "stage 2"));
    assert(strstr(hook_event_refusal("PostToolUse"), "stage 2"));
    assert(strstr(hook_event_refusal("turnSettled"), "not an event"));
    assert(strstr(hook_event_refusal("stop"), "not an event"));
    assert(hook_event_refusal(NULL) != NULL);
    printf("  PASS test_events\n");
}

/* hooks.input as the hook runs with it: what a row names, each option it
 * does not name at its default, and a row that is not what promote writes
 * read as nothing declared. */
static void test_input(void) {
    sqlite3 *db = open_seeded();
    static const struct { const char *input; int history, results, max_bytes; } cases[] = {
        { NULL, 0, 1, TURN_EDIT_INPUT_DEFAULT_BYTES },
        { "{}", 0, 1, TURN_EDIT_INPUT_DEFAULT_BYTES },
        { "{\"history\":\"none\"}", 0, 1, TURN_EDIT_INPUT_DEFAULT_BYTES },
        { "{\"history\":\"all\"}", TURN_EDIT_HISTORY_ALL, 1, TURN_EDIT_INPUT_DEFAULT_BYTES },
        { "{\"history\":5,\"history_results\":false,\"max_bytes\":2000000}", 5, 0, 2000000 },
        { "{\"history_results\":true,\"max_bytes\":99999999}", 0, 1, TURN_EDIT_INPUT_MAX_BYTES },
        { "not json", 0, 1, TURN_EDIT_INPUT_DEFAULT_BYTES },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sqlite3_stmt *st;
        assert(sqlite3_prepare_v2(db, "UPDATE hooks SET input=?1 WHERE extension_name='ext_a';",
                                  -1, &st, NULL) == SQLITE_OK);
        if (cases[i].input) sqlite3_bind_text(st, 1, cases[i].input, -1, SQLITE_STATIC);
        assert(sqlite3_step(st) == SQLITE_DONE);
        sqlite3_finalize(st);
        HookRef h;
        assert(hook_next(db, "Main", EV, 1, NULL, &h) == 1);
        assert(strcmp(h.extension, "ext_a") == 0);
        if (h.input.history != cases[i].history || h.input.history_results != cases[i].results ||
            h.input.max_bytes != cases[i].max_bytes) {
            fprintf(stderr, "FAIL input %s: got %d/%d/%d\n", cases[i].input ? cases[i].input : "NULL",
                    h.input.history, h.input.history_results, h.input.max_bytes);
            abort();
        }
    }
    db_close(db);
    printf("  PASS test_input\n");
}

int main(void) {
    TEST_INIT();
    printf("test_hook:\n");
    test_events();
    test_resolution();
    test_input();
    test_eligibility();
    test_failure_count();
    test_state_guard();
    test_recovery();
    test_resource_holder();
    test_db_clean(DB_PATH);
    printf("All hook tests passed.\n");
    return 0;
}
