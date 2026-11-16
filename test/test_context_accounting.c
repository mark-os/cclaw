/* Context accounting: the compaction trigger must see the whole request —
 * head (system prompt + tools) and provider/sent measures — not just the
 * entries' own estimates; rescaled estimates must err high (chars/3); and the
 * provider's count is the measure — the calibration factor scales every
 * estimate the cut, the mid-turn probe and the trigger read. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "db.h"
#include "test_util.h"
#include "turn_edit.h"

static sqlite3 *open_seeded(void) {
    sqlite3 *db = test_db_open(":memory:");
    if (db) test_seed_agent(db, "default");
    return db;
}

static int64_t append(sqlite3 *db, int64_t sid, Role role, const char *content,
                      int usage_in, int64_t it) {
    Message m = { .role = role, .content = (char *)content, .usage_in = usage_in,
                  .stop_reason = role == ROLE_ASSISTANT ? STOP_REASON_STOP : STOP_REASON_NONE };
    return entry_append_with_iteration(db, sid, &m, it);
}

static void add_tool(sqlite3 *db, const char *name, int param_bytes) {
    char *params = malloc(param_bytes + 1);
    memset(params, 'p', param_bytes); params[param_bytes] = 0;
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO tools(name, description, parameters_json, enabled) VALUES(?,'d',?,1)",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, params, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    free(params);
}

/* Small entries, huge head: the old trigger (entries only) never fired and
 * the provider 400'd with the head alone over budget. */
static void test_large_head_triggers(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.compaction = 1;
    cfg.context_window = 10000;      /* limit = 6000 tokens */
    int64_t it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    assert(session_needs_compaction(db, sid, &cfg) == 0);

    add_tool(db, "fat", 30000);      /* ~10000 head tokens */
    int head = context_head_estimate(db, "default", sid, &cfg, "");
    assert(head >= 10000);
    assert(session_needs_compaction(db, sid, &cfg) == 1);
    db_close(db);
    printf("  PASS test_large_head_triggers\n");
}

/* The provider's prompt_tokens is trusted when fresh — it lifts the size
 * above our under-estimate — and ignored once a compaction post-dates it,
 * or the freshly compacted session would re-fire forever. */
static void test_provider_floor_and_staleness(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.compaction = 1;
    cfg.context_window = 10000;
    int64_t it = db_next_iteration_id(db, sid);
    int64_t e1 = append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    it = db_next_iteration_id(db, sid);
    int64_t e3 = append(db, sid, ROLE_USER, "more", 0, it);
    append(db, sid, ROLE_ASSISTANT, "sure", 9000, it);   /* provider says 9000 */
    assert(session_needs_compaction(db, sid, &cfg) == 1);

    /* Compact away the middle: the 9000 figure predates the summary. */
    assert(entry_compact(db, sid, e1, e3, "summary") > 0);
    assert(session_needs_compaction(db, sid, &cfg) == 0);

    /* A new post-compaction response reinstates the provider term. */
    it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "again", 0, it);
    append(db, sid, ROLE_ASSISTANT, "ok", 9500, it);
    assert(session_needs_compaction(db, sid, &cfg) == 1);
    db_close(db);
    printf("  PASS test_provider_floor_and_staleness\n");
}

/* The sent payload (llm_responses.request_body) bounds the estimate too. */
static void test_sent_payload_floor(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.compaction = 1;
    cfg.context_window = 10000;
    int64_t it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    assert(session_needs_compaction(db, sid, &cfg) == 0);

    char *body = malloc(30010);
    strcpy(body, "{\"x\":\"");
    memset(body + 6, 'y', 30000); strcpy(body + 30006, "\"}");
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO llm_responses(session_id, iteration_id, status, request_body)"
        " VALUES(?, ?, 'ok', jsonb(?))", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_int64(s, 2, it);
    sqlite3_bind_text(s, 3, body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    free(body);
    assert(session_needs_compaction(db, sid, &cfg) == 1);
    db_close(db);
    printf("  PASS test_sent_payload_floor\n");
}

/* Estimates use chars/3 + 4, and the stored content_bytes lets a schema
 * patch rescale historical rows to the same rule. */
static void test_estimate_divisor(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    int64_t it = db_next_iteration_id(db, sid);
    int64_t id = append(db, sid, ROLE_USER, "0123456789ab", 0, it);   /* 12 bytes */
    assert(db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE id=?", id, -1)
           == TOKEN_ESTIMATE(12));
    assert(db_scalar_i64(db, "SELECT content_bytes/3 + 4 FROM entries WHERE id=?", id, -1)
           == TOKEN_ESTIMATE(12));
    db_close(db);
    printf("  PASS test_estimate_divisor\n");
}


/* ── Calibration factor ──────────────────────────────────────────── */

/* The archived request body for one iteration: JSON text of exactly n bytes. */
static void add_body(sqlite3 *db, int64_t sid, int64_t it, int n) {
    char *body = malloc((size_t)n + 1);
    memcpy(body, "{\"x\":\"", 6);
    memset(body + 6, 'y', (size_t)n - 8);
    memcpy(body + n - 2, "\"}", 3);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO llm_responses(session_id, iteration_id, status, request_body)"
        " VALUES(?, ?, 'ok', jsonb(?))", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_int64(s, 2, it);
    sqlite3_bind_text(s, 3, body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    free(body);
}

static void exec_id(sqlite3 *db, const char *sql, int64_t id) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, id);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

/* The provider's prompt count on an assistant entry; 0 = not reported. */
static void set_usage(sqlite3 *db, int64_t entry_id, int usage_in) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "UPDATE entries SET usage_in=NULLIF(?1,0) WHERE id=?2",
                              -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int(s, 1, usage_in);
    sqlite3_bind_int64(s, 2, entry_id);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

static char *filler(int n) {
    char *t = malloc((size_t)n + 1);
    memset(t, 'w', (size_t)n);
    t[n] = 0;
    return t;
}

static double factor_of(sqlite3 *db, int64_t sid, int head) {
    ContextCalibration c;
    context_calibrate(db, sid, head, &c);
    return c.factor;
}

static int near(double a, double b) { return a > b - 0.001 && a < b + 0.001; }

/* No report, a report below the estimate, one at twice it, a pruned body,
 * one predating a compaction, a model switch, a toy request, a silly one. */
static void test_factor_derivation(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    assert(near(factor_of(db, sid, 0), 1.0));            /* empty session */

    int64_t it = db_next_iteration_id(db, sid);
    int64_t u1 = append(db, sid, ROLE_USER, "hi", 0, it);
    int64_t a1 = append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    add_body(db, sid, it, 30000);                        /* estimate 10000 */
    assert(near(factor_of(db, sid, 0), 1.0));            /* no report */

    set_usage(db, a1, 8000);
    ContextCalibration c;
    context_calibrate(db, sid, 0, &c);
    assert(near(c.factor, 1.0) && c.provider == 8000 && c.sent == 10000);

    set_usage(db, a1, 20000);
    assert(near(factor_of(db, sid, 0), 2.0));

    set_usage(db, a1, 95000);
    assert(near(factor_of(db, sid, 0), (double)CHARS_PER_TOKEN));   /* clamped */

    /* Pruned body: the branch before the response, plus the head. */
    exec_id(db, "UPDATE llm_responses SET request_body=NULL WHERE session_id=?", sid);
    set_usage(db, a1, 20000);
    int before = (int)db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE id=?", u1, -1);
    assert(near(factor_of(db, sid, 10000 - before), 2.0));
    assert(near(factor_of(db, sid, 0), 1.0));            /* too small to calibrate on */

    /* A toy request: under the minimum sample, whatever the ratio. */
    add_body(db, sid, it, 3000);
    set_usage(db, a1, 2500);
    assert(near(factor_of(db, sid, 0), 1.0));

    /* Model switch: the newest response wins, in both directions. */
    int64_t it2 = db_next_iteration_id(db, sid);
    int64_t u2 = append(db, sid, ROLE_USER, "more", 0, it2);
    int64_t a2 = append(db, sid, ROLE_ASSISTANT, "sure", 0, it2);
    exec_id(db, "UPDATE entries SET model='other' WHERE id=?", a2);
    add_body(db, sid, it2, 30000);
    set_usage(db, a1, 20000);
    set_usage(db, a2, 12000);
    assert(near(factor_of(db, sid, 0), 1.2));
    set_usage(db, a2, 0);
    assert(near(factor_of(db, sid, 0), 1.0));            /* new model reports nothing */

    /* A report older than the newest compaction counts for nothing. */
    set_usage(db, a2, 24000);
    assert(near(factor_of(db, sid, 0), 2.4));
    assert(entry_compact(db, sid, u1, u2, "summary") > 0);
    assert(near(factor_of(db, sid, 0), 1.0));
    db_close(db);
    printf("  PASS test_factor_derivation\n");
}

/* Six turns of ~1000 estimated tokens each fit a 7200-token budget; once
 * the provider says it counts twice what we estimate, they do not. */
static void test_cut_keeps_fewer_turns(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    char *text = filler(3000);
    int64_t it = 0, last = 0;
    for (int i = 0; i < 6; i++) {
        it = db_next_iteration_id(db, sid);
        append(db, sid, ROLE_USER, text, 0, it);
        last = append(db, sid, ROLE_ASSISTANT, "ok", 0, it);
    }
    free(text);
    add_body(db, sid, it, 18000);                        /* estimate 6000 */
    Config cfg = {0};
    cfg.context_window = 12000;                          /* budget 7200 */
    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(plan.cut == 0 && near(plan.factor, 1.0));
    context_plan_free(&plan);

    set_usage(db, last, 12000);
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(near(plan.factor, 2.0));
    /* budget/2 = 3600 estimated tokens: three whole turns of 1008, then the
     * cut lands inside the fourth and snaps back to keep it (one-turn
     * overshoot, as ever) — four of six turns. */
    assert(plan.count == 12 && plan.cut == 4);
    context_plan_free(&plan);
    db_close(db);
    printf("  PASS test_cut_keeps_fewer_turns\n");
}

/* The probe compares the turn against the window in provider tokens. */
static void test_probe_trips_earlier(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    int64_t it = db_next_iteration_id(db, sid);
    char *ask = filler(7000), *out = filler(12000);
    append(db, sid, ROLE_USER, ask, 0, it);
    int64_t a1 = append(db, sid, ROLE_ASSISTANT, "looking", 0, it);
    add_body(db, sid, it, 7200);                         /* estimate 2400 */
    Message r = { .role = ROLE_TOOL, .content = out };
    entry_append_with_iteration(db, sid, &r, it);
    free(ask); free(out);

    int turn = 0;
    assert(context_turn_overflows(db, sid, 8000, 0, &turn) == 0);
    assert(turn > 6000 && turn < 8000);
    set_usage(db, a1, 4800);
    int scaled = 0;
    assert(context_turn_overflows(db, sid, 8000, 0, &scaled) == 1);
    assert(scaled == turn * 2);
    db_close(db);
    printf("  PASS test_probe_trips_earlier\n");
}

/* Without an edit the trigger only rises: the provider and sent floors hold
 * as before, and the estimate and the last turn are both scaled up. Here the
 * provider count alone sits under the old limit; the next turn, scaled the
 * same way, no longer fits. */
static void test_trigger_no_edit_only_rises(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.compaction = 1;
    int head = context_head_estimate(db, "default", sid, &cfg, NULL);
    int64_t it = db_next_iteration_id(db, sid);
    char *big = filler(9000);                            /* ~3000 estimated */
    append(db, sid, ROLE_USER, big, 0, it);
    int64_t a1 = append(db, sid, ROLE_ASSISTANT, "ok", 0, it);
    free(big);
    int turn = (int)db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=?", sid, 0);
    int est = turn + head;
    add_body(db, sid, it, est * 3);                      /* sent == estimate */
    /* limit = 1.3E + 1.15T: over the old bar (1.3E vs L - T), under the new
     * (1.3E vs L - 1.3T). */
    int limit = (int)(1.3 * est + 1.15 * turn);
    cfg.context_window = (int)(limit / 0.6f) + 1;
    assert(session_needs_compaction(db, sid, &cfg) == 0);   /* no report: E < L - T */
    set_usage(db, a1, (int)(1.3 * est));
    ContextCalibration c;
    context_calibrate(db, sid, head, &c);
    assert(near(c.factor, 1.3) && c.shrink == 0);
    assert(c.provider < limit - turn);                   /* the old trigger: no */
    assert(session_needs_compaction(db, sid, &cfg) == 1);
    db_close(db);
    printf("  PASS test_trigger_no_edit_only_rises\n");
}

/* A turn edit that drops the turn's big tool output: the trigger that
 * follows sees the shrink, though the newest provider count predates it. */
static void test_trigger_sees_turn_edit(void) {
    sqlite3 *db = open_seeded();
    assert(sqlite3_exec(db, "INSERT INTO tools(name, droppable) VALUES ('look',1);",
                        NULL, NULL, NULL) == SQLITE_OK);
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.compaction = 1;
    int head = context_head_estimate(db, "default", sid, &cfg, NULL);

    int64_t it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    Message u = { .role = ROLE_USER, .content = "go" };
    entry_append_with_iteration(db, sid, &u, 0);
    int64_t it1 = db_next_iteration_id(db, sid);
    entry_append_typed(db, sid, it1, "assistant_message", 0, "one", NULL, NULL, 0,
                       STOP_REASON_NONE, NULL, 0, 0, 0);
    int64_t c1 = entry_append_typed(db, sid, it1, "tool_call", 0, "{}", "c1", "look", 0,
                                    STOP_REASON_NONE, NULL, 0, 0, 0);
    exec_id(db, "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
                    " SELECT session_id, id, 'c1', 'look', 'done' FROM entries WHERE id=?", c1);
    char *out = filler(24000);                           /* ~8000 estimated */
    entry_append_typed(db, sid, it1, "tool_result", 0, out, "c1", "look", 0,
                       STOP_REASON_NONE, NULL, 0, 0, 0);
    free(out);
    int64_t it2 = db_next_iteration_id(db, sid);
    int sent = (int)db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=?", sid, 0) + head;
    int64_t fin = entry_append_typed(db, sid, it2, "assistant_message", 0, "done", NULL,
                                     NULL, 0, STOP_REASON_NONE, NULL, 2 * sent, 0, 0);
    add_body(db, sid, it2, sent * 3);
    assert(fin > 0);

    cfg.context_window = (int)((2 * head + 6000) / 0.6f);
    ContextCalibration c;
    context_calibrate(db, sid, head, &c);
    assert(near(c.factor, 2.0) && c.shrink == 0);
    assert(session_needs_compaction(db, sid, &cfg) == 1);

    char *err = NULL;
    assert(turn_edit_apply(db, sid, "{\"edits\":[{\"op\":\"drop\",\"call_id\":\"c1\"}]}",
                           "hook:t", &err) == TURN_EDIT_APPLIED);
    context_calibrate(db, sid, head, &c);
    assert(near(c.factor, 2.0) && c.shrink > 8000);      /* call + its result */
    assert(session_needs_compaction(db, sid, &cfg) == 0);
    db_close(db);
    printf("  PASS test_trigger_sees_turn_edit\n");
}

int main(void) {
    TEST_INIT();
    printf("test_context_accounting:\n");
    test_large_head_triggers();
    test_provider_floor_and_staleness();
    test_sent_payload_floor();
    test_estimate_divisor();
    test_factor_derivation();
    test_cut_keeps_fewer_turns();
    test_probe_trips_earlier();
    test_trigger_no_edit_only_rises();
    test_trigger_sees_turn_edit();
    printf("All context accounting tests passed.\n");
    return 0;
}
