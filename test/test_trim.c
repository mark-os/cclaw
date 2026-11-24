/* C's trim (turn_edit.c, turn_edit_trim): the fallback that keeps a session
 * under its trim threshold with no model call — each of its four steps, the
 * rules every edit obeys (pairing, unfinished calls, the record, originals
 * untouched), the governing limits and the schema's refusals. Sizes here are
 * estimates (no reports): an entry of n bytes costs TOKEN_ESTIMATE(n) tokens. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "advance.h"
#include "config.h"
#include "context.h"
#include "db.h"
#include "db_response.h"
#include "test_util.h"
#include "turn_edit.h"

static char *filler(int n, char c) {
    char *t = malloc((size_t)n + 1);
    memset(t, c, (size_t)n);
    t[n] = 0;
    return t;
}

/* An entry of `bytes` bytes of `c`: TOKEN_ESTIMATE(bytes) tokens. */
static int64_t add(sqlite3 *db, int64_t sid, int64_t it, const char *type, int bytes,
                   const char *call_id, StopReason stop) {
    char *t = filler(bytes, 'w');
    int64_t id = entry_append_typed(db, sid, it, type, 0, t, call_id,
                                    call_id ? "look" : NULL, 0, stop, NULL, 0, 0, 0);
    free(t);
    return id;
}

/* One iteration with one call and its result; returns the message's id. */
static int64_t step(sqlite3 *db, int64_t sid, int msg_bytes, int result_bytes) {
    int64_t it = db_next_iteration_id(db, sid);
    char cid[32];
    snprintf(cid, sizeof(cid), "c%lld", (long long)it);
    int64_t a = add(db, sid, it, "assistant_message", msg_bytes, NULL, STOP_REASON_TOOL_USE);
    add(db, sid, it, "tool_call", 30, cid, STOP_REASON_NONE);
    add(db, sid, it, "tool_result", result_bytes, cid, STOP_REASON_NONE);
    return a;
}

static int64_t answer(sqlite3 *db, int64_t sid, int bytes) {
    return add(db, sid, db_next_iteration_id(db, sid), "assistant_message", bytes, NULL,
               STOP_REASON_STOP);
}

static int64_t user(sqlite3 *db, int64_t sid, int bytes) {
    return add(db, sid, 0, "user_message", bytes, NULL, STOP_REASON_NONE);
}

/* A finished turn: question, one step, answer. Returns the question's id. */
static int64_t turn(sqlite3 *db, int64_t sid, int bytes) {
    int64_t u = user(db, sid, bytes);
    step(db, sid, bytes, bytes);
    answer(db, sid, bytes);
    return u;
}

static int size_now(sqlite3 *db, int64_t sid) {
    ContextPlan m = {0};
    assert(context_measure(db, sid, 0, &m) == 0);
    int t = m.head;
    for (int i = 0; i < m.count; i++) t += m.entries[i].tokens;
    context_plan_free(&m);
    return t;
}

static int on_branch(sqlite3 *db, int64_t sid, int64_t id) {
    ContextPlan m = {0};
    assert(context_measure(db, sid, 0, &m) == 0);
    int found = 0;
    for (int i = 0; i < m.count; i++) found |= m.entries[i].id == id;
    context_plan_free(&m);
    return found;
}

static int64_t q1(sqlite3 *db, const char *sql, int64_t a) {
    return db_scalar_i64(db, sql, a, -1);
}

static sqlite3 *open_db(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    test_seed_agent(db, "default");
    return db;
}

static ContextLimits limits(int threshold, int target) {
    ContextLimits l = { .window = 1000000, .threshold = threshold, .target = target };
    return l;
}

/* The note a trim wrote: its id and where it sits. */
static int64_t note_of(sqlite3 *db, int64_t sid) {
    return q1(db, "SELECT id FROM entries WHERE session_id=?1 AND type='system'"
                  " AND json_extract(data,'$.source')='" TRIM_SOURCE "'"
                  " ORDER BY id DESC LIMIT 1", sid);
}

/* ── Step 0: under the threshold nothing happens ─────────────────── */

static void test_under_threshold(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    turn(db, sid, 300);
    int before = size_now(db, sid);
    int64_t n = q1(db, "SELECT COUNT(*) FROM entries WHERE session_id=?", sid);
    ContextLimits l = limits(before, before / 2);
    TrimReport r;
    assert(turn_edit_trim(db, sid, 0, &l, &r) == TRIM_UNDER);
    assert(r.turns == 0 && r.iterations == 0 && r.note_id == 0);
    assert(q1(db, "SELECT COUNT(*) FROM entries WHERE session_id=?", sid) == n);
    db_close(db);
}

/* ── Step 1: the oldest whole turns go, down to the target ────────── */

static void test_drop_turns(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    int64_t t[5];
    for (int i = 0; i < 5; i++) t[i] = turn(db, sid, 300);
    int one = TOKEN_ESTIMATE(300) * 4 + TOKEN_ESTIMATE(30);   /* a turn's size */
    int total = size_now(db, sid);
    assert(total == 5 * one);
    int64_t leaf = q1(db, "SELECT leaf_id FROM sessions WHERE id=?", sid);

    /* Over 4 turns' worth; the target leaves room for 2. */
    ContextLimits l = limits(4 * one, 2 * one + 10);
    TrimReport r;
    assert(turn_edit_trim(db, sid, 0, &l, &r) == TRIM_APPLIED);
    assert(r.turns == 3 && r.iterations == 0);
    assert(r.before == total && r.after <= 2 * one + 10);

    /* The newest turn and the one before it stay; the three oldest left. */
    assert(!on_branch(db, sid, t[0]) && !on_branch(db, sid, t[2]));
    assert(on_branch(db, sid, t[3]) && on_branch(db, sid, t[4]));
    assert(q1(db, "SELECT leaf_id FROM sessions WHERE id=?", sid) == leaf);

    /* One note, at the head of the first kept turn, in that turn. */
    int64_t note = note_of(db, sid);
    assert(note == r.note_id && on_branch(db, sid, note));
    assert(q1(db, "SELECT parent_id FROM entries WHERE id=?", t[3]) == note);
    assert(q1(db, "SELECT parent_id FROM entries WHERE id=?", note) == -1);
    assert(q1(db, "SELECT turn_id FROM entries WHERE id=?", note) ==
           q1(db, "SELECT turn_id FROM entries WHERE id=?", t[3]));
    assert(size_now(db, sid) == r.after + TOKEN_ESTIMATE((int)q1(db,
               "SELECT length(content) FROM entries WHERE id=?", note)));

    /* Originals: still there, content intact; the re-pointed one remembers. */
    assert(q1(db, "SELECT COUNT(*) FROM entries WHERE id=? AND length(content)=300", t[0]) == 1);
    assert(q1(db, "SELECT original_parent_id FROM entries WHERE id=?", t[3]) > 0);
    assert(q1(db, "SELECT original_parent_id IS NULL FROM entries WHERE id=?", t[1]) == 1);
    db_close(db);
}

/* ── Step 2: then the oldest iterations of the running turn ────────── */

static void test_drop_iterations(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    int64_t old = turn(db, sid, 60);
    int64_t u1 = user(db, sid, 60), u2 = user(db, sid, 60);   /* two openers */
    int64_t s[6];
    for (int i = 0; i < 6; i++) s[i] = step(db, sid, 600, 900);
    int it_size = TOKEN_ESTIMATE(600) + TOKEN_ESTIMATE(30) + TOKEN_ESTIMATE(900);
    int openers = 2 * TOKEN_ESTIMATE(60);

    /* Room for the openers and two iterations: the old turn and the four
     * oldest iterations go. */
    ContextLimits l = limits(openers + 3 * it_size, openers + 2 * it_size + 5);
    TrimReport r;
    assert(turn_edit_trim(db, sid, 0, &l, &r) == TRIM_APPLIED);
    assert(r.turns == 1 && r.iterations == 4);
    assert(!on_branch(db, sid, old));
    assert(on_branch(db, sid, u1) && on_branch(db, sid, u2));
    for (int i = 0; i < 4; i++) assert(!on_branch(db, sid, s[i]));
    assert(on_branch(db, sid, s[4]) && on_branch(db, sid, s[5]));

    /* Calls left with their results, whole iterations at a time: every
     * call on the branch has its result there and vice versa. */
    assert(q1(db, "WITH RECURSIVE b(id) AS (SELECT leaf_id FROM sessions WHERE id=?1"
                  " UNION ALL SELECT e.parent_id FROM entries e JOIN b ON e.id=b.id)"
                  " SELECT COUNT(*) FROM entries c JOIN b ON c.id=b.id"
                  " WHERE c.type IN ('tool_call','tool_result') AND"
                  " (SELECT COUNT(*) FROM entries d JOIN b b2 ON d.id=b2.id"
                  "   WHERE d.tool_call_id=c.tool_call_id) <> 2", sid) == 0);

    /* The note sits after the openers, before the first kept iteration —
     * in the running turn — and says both. */
    int64_t note = r.note_id;
    assert(q1(db, "SELECT parent_id FROM entries WHERE id=?", note) == u2);
    assert(q1(db, "SELECT parent_id FROM entries WHERE id=?", s[4]) == note);
    assert(q1(db, "SELECT turn_id FROM entries WHERE id=?", note) ==
           q1(db, "SELECT turn_id FROM entries WHERE id=?", u1));
    assert(q1(db, "SELECT content LIKE '1 earlier turn and 4 earlier steps of this turn"
                  " were dropped%' FROM entries WHERE id=?", note) == 1);
    /* The old turn left from the front: the first opener is the root now. */
    assert(q1(db, "SELECT parent_id FROM entries WHERE id=?", u1) == -1);
    db_close(db);
}

/* A long turn trimmed again: the new note replaces the earlier one among the
 * rows that opened the turn, and carries the running total. */
static void test_notes_fold(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    turn(db, sid, 60);
    user(db, sid, 60);
    for (int i = 0; i < 4; i++) step(db, sid, 600, 900);
    int it_size = TOKEN_ESTIMATE(600) + TOKEN_ESTIMATE(30) + TOKEN_ESTIMATE(900);
    ContextLimits l = limits(3 * it_size, 2 * it_size + 100);
    TrimReport r1, r2;
    assert(turn_edit_trim(db, sid, 0, &l, &r1) == TRIM_APPLIED);
    assert(r1.turns == 1 && r1.iterations == 2);
    for (int i = 0; i < 3; i++) step(db, sid, 600, 900);
    assert(turn_edit_trim(db, sid, 0, &l, &r2) == TRIM_APPLIED);
    assert(r2.turns == 0 && r2.iterations == 3);
    assert(!on_branch(db, sid, r1.note_id) && on_branch(db, sid, r2.note_id));
    assert(q1(db, "SELECT content LIKE '1 earlier turn and 5 earlier steps of this turn"
                  " were dropped%' FROM entries WHERE id=?", r2.note_id) == 1);
    /* Its own record counts its own drop, and lists the note it replaced. */
    assert(q1(db, "SELECT json_extract(data,'$.turn_edits[0].dropped.iterations')"
                  " FROM entries WHERE id=?", r2.note_id) == 3);
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT EXISTS(SELECT 1 FROM json_each((SELECT data FROM"
             " entries WHERE id=%lld), '$.turn_edits[0].left_branch') WHERE value=?1)",
             (long long)r2.note_id);
    assert(q1(db, sql, r1.note_id) == 1);

    /* A third trim carries the running total the second note said (1 + 5),
     * not the second trim's own drop (found live: the third note said 12
     * after 6 + 6 + 6). */
    TrimReport r3;
    for (int i = 0; i < 3; i++) step(db, sid, 600, 900);
    assert(turn_edit_trim(db, sid, 0, &l, &r3) == TRIM_APPLIED);
    assert(r3.turns == 0 && r3.iterations == 3);
    assert(!on_branch(db, sid, r2.note_id) && on_branch(db, sid, r3.note_id));
    assert(q1(db, "SELECT content LIKE '1 earlier turn and 8 earlier steps of this turn"
                  " were dropped%' FROM entries WHERE id=?", r3.note_id) == 1);
    assert(q1(db, "SELECT json_extract(data,'$.turn_edits[0].dropped.iterations')"
                  " FROM entries WHERE id=?", r3.note_id) == 3);
    db_close(db);
}

/* ── Step 3: the record, on the note ───────────────────────────────── */

static void test_record(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    int64_t t0 = turn(db, sid, 300);
    turn(db, sid, 300);
    int one = size_now(db, sid) / 2;
    ContextLimits l = limits(one + 1, one);
    TrimReport r;
    assert(turn_edit_trim(db, sid, 0, &l, &r) == TRIM_APPLIED);
    assert(r.turns == 1);
    int64_t n = r.note_id;
    assert(q1(db, "SELECT json_extract(data,'$.turn_edits[0].by') = '" TRIM_SOURCE "'"
                  " FROM entries WHERE id=?", n) == 1);
    assert(q1(db, "SELECT json_array_length(data,'$.turn_edits[0].left_branch')"
                  " FROM entries WHERE id=?", n) == 5);
    assert(q1(db, "SELECT json_extract(data,'$.turn_edits[0].left_branch[0]')"
                  " FROM entries WHERE id=?", n) == t0);
    assert(q1(db, "SELECT json_extract(data,'$.turn_edits[0].tokens.before')"
                  " FROM entries WHERE id=?", n) == r.before);
    assert(q1(db, "SELECT json_extract(data,'$.turn_edits[0].tokens.after')"
                  " FROM entries WHERE id=?", n) == r.after);
    assert(q1(db, "SELECT json_extract(data,'$.turn_edits[0].dropped.turns')"
                  " FROM entries WHERE id=?", n) == 1);
    assert(q1(db, "SELECT json_extract(data,'$.turn_edits[0].limits.threshold')"
                  " FROM entries WHERE id=?", n) == one + 1);
    /* Sent as an ordinary system note, the runtime's own words. */
    assert(q1(db, "SELECT role=0 AND iteration_id=0 FROM entries WHERE id=?", n) == 1);
    db_close(db);
}

/* ── Step 4: still over — the newest iteration alone is too big ────── */

static void test_still_over(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    turn(db, sid, 300);
    user(db, sid, 30);
    step(db, sid, 30, 30);
    int64_t big = step(db, sid, 30, 9000);       /* ~3000 tokens */
    ContextLimits l = limits(2000, 1000);
    TrimReport r;
    assert(turn_edit_trim(db, sid, 0, &l, &r) == TRIM_OVER);
    /* Everything removable went and the drop was kept; the rest stays. */
    assert(r.turns == 1 && r.iterations == 1 && r.note_id > 0);
    assert(on_branch(db, sid, big) && r.after > 2000);
    assert(note_of(db, sid) == r.note_id);

    /* Nothing left to drop: still over, nothing written. */
    int64_t n = q1(db, "SELECT COUNT(*) FROM entries WHERE session_id=?", sid);
    assert(turn_edit_trim(db, sid, 0, &l, &r) == TRIM_OVER);
    assert(r.turns == 0 && r.iterations == 0 && r.note_id == 0);
    assert(q1(db, "SELECT COUNT(*) FROM entries WHERE session_id=?", sid) == n);
    db_close(db);
}

/* ── An unfinished call blocks whatever holds it ───────────────────── */

static void call_status(sqlite3 *db, int64_t sid, int64_t asst, const char *status) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
        " SELECT session_id, id, tool_call_id, tool_name, ?3 FROM entries"
        "  WHERE session_id=?1 AND id=?2+1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_int64(s, 2, asst);
    sqlite3_bind_text(s, 3, status, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

static void test_unfinished_blocks(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    int64_t t0 = user(db, sid, 300);
    int64_t job = step(db, sid, 300, 300);         /* a background job, still running */
    call_status(db, sid, job, "running");
    answer(db, sid, 300);
    int64_t t1 = turn(db, sid, 300);
    user(db, sid, 30);
    int64_t s0 = step(db, sid, 600, 600);
    call_status(db, sid, s0, "pending");
    int64_t s1 = step(db, sid, 600, 600);
    int64_t s2 = step(db, sid, 30, 30);

    ContextLimits l = limits(100, 50);             /* everything that may, goes */
    TrimReport r;
    assert(turn_edit_trim(db, sid, 0, &l, &r) == TRIM_OVER);
    assert(on_branch(db, sid, t0) && on_branch(db, sid, job));   /* blocked turn */
    assert(!on_branch(db, sid, t1));                            /* the next one went */
    assert(on_branch(db, sid, s0) && !on_branch(db, sid, s1));
    assert(r.turns == 1 && r.iterations == 1);
    /* Iterations went, so the note is at the running turn's gap. */
    assert(q1(db, "SELECT parent_id FROM entries WHERE id=?", s2) == r.note_id);
    assert(q1(db, "SELECT turn_id FROM entries WHERE id=?", r.note_id) ==
           q1(db, "SELECT turn_id FROM entries WHERE id=?", s0));
    db_close(db);
}

/* ── It works whatever the two numbers are ─────────────────────────── */

static void test_any_numbers(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    for (int i = 0; i < 4; i++) turn(db, sid, 300);
    int total = size_now(db, sid);
    /* A target at or over the threshold: the session still ends under it. */
    ContextLimits l = limits(total - 1, total * 2);
    TrimReport r;
    assert(turn_edit_trim(db, sid, 0, &l, &r) == TRIM_APPLIED);
    assert(r.turns == 1 && r.after <= total - 1);
    db_close(db);
}

/* ── The governing entry and its defaults ──────────────────────────── */

static void exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "sql failed: %s\n  %s\n", err ? err : "?", sql);
        assert(0);
    }
}

static int refused(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK;
}

static void seed_models(sqlite3 *db) {
    exec(db, "INSERT INTO providers(name, base_url, endpoint_type, api_key_env)"
             " VALUES('p','http://x','openai','K');"
             "INSERT INTO models(id, provider_name, model, context_window)"
             " VALUES('p/big','p','big',200000), ('p/small','p','small',8000),"
             "       ('p/nowin','p','nowin',NULL);");
}

static void test_limits(void) {
    sqlite3 *db = open_db();
    seed_models(db);
    ContextLimits l;
    context_limits(db, "default", 128000, &l);         /* no list: the default */
    assert(l.window == 128000 && l.threshold == 64000 && l.target == 32000);

    exec(db, "INSERT INTO agent_models(agent_name, model_id, pos)"
             " VALUES('default','p/small',0), ('default','p/big',1);");
    context_limits(db, "default", 128000, &l);         /* the first entry governs */
    assert(l.window == 8000 && l.threshold == 4000 && l.target == 2000);

    exec(db, "UPDATE agent_models SET trim_threshold=6000 WHERE pos=0;");
    context_limits(db, "default", 128000, &l);
    assert(l.threshold == 6000 && l.target == 3000);
    exec(db, "UPDATE agent_models SET prune_target=1000 WHERE pos=0;");
    context_limits(db, "default", 128000, &l);
    assert(l.threshold == 6000 && l.target == 1000);

    /* A disabled model does not govern; the next enabled one does. */
    exec(db, "UPDATE models SET status='disabled' WHERE id='p/small';");
    context_limits(db, "default", 128000, &l);
    assert(l.window == 200000 && l.threshold == 100000 && l.target == 50000);
    db_close(db);
}

static void test_schema_refusals(void) {
    sqlite3 *db = open_db();
    seed_models(db);
    exec(db, "INSERT INTO agent_models(agent_name, model_id, pos, trim_threshold, prune_target)"
             " VALUES('default','p/small',0,8000,4000);");      /* at the window: fine */
    /* prune_target at or above trim_threshold */
    assert(refused(db, "UPDATE agent_models SET prune_target=8000 WHERE pos=0;"));
    assert(refused(db, "UPDATE agent_models SET prune_target=9000 WHERE pos=0;"));
    /* trim_threshold above the model's window, on insert and on update */
    assert(refused(db, "UPDATE agent_models SET trim_threshold=8001 WHERE pos=0;"));
    assert(refused(db, "INSERT INTO agent_models(agent_name, model_id, pos, trim_threshold)"
                       " VALUES('default','p/small',1,9000);"));
    /* moving the entry to a smaller model re-checks it */
    exec(db, "UPDATE agent_models SET trim_threshold=100000, prune_target=NULL,"
             " model_id='p/big' WHERE pos=0;");
    assert(refused(db, "UPDATE agent_models SET model_id='p/small' WHERE pos=0;"));
    /* not positive */
    assert(refused(db, "UPDATE agent_models SET trim_threshold=0 WHERE pos=0;"));
    assert(refused(db, "UPDATE agent_models SET prune_target=-1 WHERE pos=0;"));
    /* an unknown window cannot be exceeded; NULLs are the defaults */
    exec(db, "INSERT INTO agent_models(agent_name, model_id, pos, trim_threshold)"
             " VALUES('default','p/nowin',1,5000000);");
    exec(db, "UPDATE agent_models SET trim_threshold=NULL, prune_target=NULL;");
    db_close(db);
}

/* ── The C check at the end of an iteration (advance_session) ───────── */

static void to_tool_running(sqlite3 *db, int64_t sid) {
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "tool_running") == 0);
}

static void test_advance_trims_mid_turn(void) {
    sqlite3 *db = open_db();
    seed_models(db);
    exec(db, "INSERT INTO agent_models(agent_name, model_id, pos)"
             " VALUES('default','p/big',0);");
    int64_t sid = session_create(db, "s", "default", -1, 0);
    /* The head (system prompt, tools) is whatever this agent's is; the
     * numbers sit above it. */
    Config *cfg = config_load(db);
    int head = context_head_estimate(db, "default", sid, cfg, NULL);
    config_free(cfg);
    char sql[256];
    snprintf(sql, sizeof(sql), "UPDATE agent_models SET trim_threshold=%d,"
             " prune_target=%d;", head + 4000, head + 2000);
    exec(db, sql);
    user(db, sid, 60);
    int64_t s[8];
    for (int i = 0; i < 8; i++) s[i] = step(db, sid, 30, 2400);   /* ~800 tokens each */
    exec(db, "UPDATE tool_calls SET status='done';");
    to_tool_running(db, sid);
    AdvanceOutput out = advance_session(db, sid, 50);
    assert(out.action == ADVANCE_DISPATCH_LLM);
    assert(!on_branch(db, sid, s[0]) && on_branch(db, sid, s[7]));
    assert(note_of(db, sid) > 0);

    /* The newest iteration alone over the threshold: the turn stops, with
     * the notice where the channel sees it. */
    step(db, sid, 30, 18000);
    exec(db, "UPDATE tool_calls SET status='done';");
    assert(session_set_state(db, sid, "tool_running") == 0);
    out = advance_session(db, sid, 50);
    assert(out.action == ADVANCE_DONE);
    assert(q1(db, "SELECT content LIKE '%context limit reached%' AND stop_reason=4"
                  " FROM entries WHERE id=(SELECT leaf_id FROM sessions WHERE id=?)",
              sid) == 1);
    assert(q1(db, "SELECT state='idle' FROM sessions WHERE id=?", sid) == 1);
    db_close(db);
}

/* ── Accounting: no difference across a trim ───────────────────────── */

static int64_t ingest(sqlite3 *db, int64_t sid, int in, int calls) {
    char body[1024], tc[256] = "";
    int64_t it = db_next_iteration_id(db, sid);
    if (calls)
        snprintf(tc, sizeof(tc), ",\"tool_calls\":[{\"id\":\"k%lld\",\"type\":\"function\","
                 "\"function\":{\"name\":\"look\",\"arguments\":\"{}\"}}]", (long long)it);
    snprintf(body, sizeof(body),
             "{\"choices\":[{\"finish_reason\":\"%s\",\"message\":{\"role\":\"assistant\","
             "\"content\":\"a reply of some ordinary length, forty-odd bytes\"%s}}],"
             "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":20}}",
             calls ? "tool_calls" : "stop", tc, in);
    TypedIngestResult ir;
    assert(db_ingest_response(db, sid, it, "m", ENDPOINT_OPENAI, body, NULL, 0, &ir)
           == LLM_RESP_OK);
    if (calls) {
        char cid[32];
        snprintf(cid, sizeof(cid), "k%lld", (long long)it);
        exec(db, "UPDATE tool_calls SET status='done';");
        add(db, sid, it, "tool_result", 3000, cid, STOP_REASON_NONE);
    }
    return ir.assistant_entry_id;
}

static void test_no_difference_across_trim(void) {
    sqlite3 *db = open_db();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    turn(db, sid, 3000);
    user(db, sid, 300);
    ingest(db, sid, 5000, 1);
    int64_t r1 = q1(db, "SELECT leaf_id FROM sessions WHERE id=?", sid);
    ContextLimits l = limits(size_now(db, sid) - 1, 100);
    TrimReport r;
    assert(turn_edit_trim(db, sid, 0, &l, &r) != TRIM_ERROR && r.turns == 1);
    /* A report that would fit "the previous one plus what was appended" —
     * but the trim came between them, so no share is booked from it. */
    ingest(db, sid, 5000 + 20 + 1004, 0);
    assert(q1(db, "SELECT tokens IS NULL FROM entries WHERE id=?", r1) == 1);
    db_close(db);
}

/* The same for a script's edit between iterations, whatever it touches: an
 * earlier turn (the record lands on the running turn's leaf) or the newest
 * result itself (on the stand-in that replaced it). */
static void test_no_difference_across_script_edit(void) {
    const char *replies[] = {
        "{\"edits\":[{\"op\":\"drop_turns\",\"from_turn\":1,\"to_turn\":1}]}",
        "{\"edits\":[{\"op\":\"replace\",\"call_id\":\"%s\",\"text\":\"short\"}]}",
    };
    for (int k = 0; k < 2; k++) {
        sqlite3 *db = open_db();
        int64_t sid = session_create(db, "s", "default", -1, 0);
        turn(db, sid, 3000);
        user(db, sid, 300);
        ingest(db, sid, 5000, 1);
        char reply[256], cid[64];
        sqlite3_stmt *st;
        assert(sqlite3_prepare_v2(db, "SELECT tool_call_id FROM entries WHERE id="
                                      "(SELECT leaf_id FROM sessions WHERE id=?1)",
                                  -1, &st, NULL) == SQLITE_OK);
        sqlite3_bind_int64(st, 1, sid);
        assert(sqlite3_step(st) == SQLITE_ROW);
        snprintf(cid, sizeof(cid), "%s", sqlite3_column_text(st, 0));
        sqlite3_finalize(st);
        snprintf(reply, sizeof(reply), replies[k], cid);
        char *err = NULL;
        assert(turn_edit_apply(db, sid, reply, "hook:x", &err) == TURN_EDIT_APPLIED);
        int64_t leaf = q1(db, "SELECT leaf_id FROM sessions WHERE id=?", sid);
        ingest(db, sid, 5000 + 20 + 1004, 0);
        assert(q1(db, "SELECT tokens IS NULL FROM entries WHERE id=?", leaf) == 1);
        db_close(db);
    }
}

int main(void) {
    TEST_INIT();
    printf("test_trim:\n");
    RUN(test_under_threshold);
    RUN(test_drop_turns);
    RUN(test_drop_iterations);
    RUN(test_notes_fold);
    RUN(test_record);
    RUN(test_still_over);
    RUN(test_unfinished_blocks);
    RUN(test_any_numbers);
    RUN(test_limits);
    RUN(test_schema_refusals);
    RUN(test_advance_trims_mid_turn);
    RUN(test_no_difference_across_trim);
    RUN(test_no_difference_across_script_edit);
    printf("All trim tests passed.\n");
    return 0;
}
