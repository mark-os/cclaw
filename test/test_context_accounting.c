/* Context accounting: the compaction trigger must see the whole request —
 * head (system prompt + tools) and provider/sent measures — not just the
 * entries' own estimates; rescaled estimates must err high (chars/3); and the
 * provider's count is the measure — segments between reports are sized by
 * the difference of the provider's counts, the estimate the fallback. */
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


/* ── Measured sizes: the provider's count, the estimate the fallback ── */

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

static char *filler(int n) {
    char *t = malloc((size_t)n + 1);
    memset(t, 'w', (size_t)n);
    t[n] = 0;
    return t;
}

static int64_t user_msg(sqlite3 *db, int64_t sid, int bytes) {
    char *t = filler(bytes);
    int64_t id = entry_append_typed(db, sid, 0, "user_message", 0, t, NULL, NULL, 0,
                                    STOP_REASON_NONE, NULL, 0, 0, 0);
    free(t);
    return id;
}

/* One iteration as db_ingest_response writes it: the response (usage_in /
 * usage_out as reported, 0 = not), then — when result_bytes > 0 — one tool
 * call and its result. Returns the response's entry id. */
static int64_t respond(sqlite3 *db, int64_t sid, const char *model, int in, int out,
                       int result_bytes) {
    int64_t it = db_next_iteration_id(db, sid);
    int64_t a = entry_append_typed(db, sid, it, "assistant_message", 0, "ok", NULL, NULL, 0,
                                   result_bytes ? STOP_REASON_TOOL_USE : STOP_REASON_STOP,
                                   model, in, out, 0);
    if (result_bytes > 0) {
        char cid[32];
        snprintf(cid, sizeof(cid), "c%lld", (long long)it);
        entry_append_typed(db, sid, it, "tool_call", 1, "{}", cid, "look", 0,
                           STOP_REASON_NONE, NULL, 0, 0, 0);
        char *t = filler(result_bytes);
        entry_append_typed(db, sid, it, "tool_result", 2, t, cid, "look", 0,
                           STOP_REASON_NONE, NULL, 0, 0, 0);
        free(t);
    }
    return a;
}

/* Sum of token_estimate over entries with from <= id < to. */
static int est_ids(sqlite3 *db, int64_t sid, int64_t from, int64_t to) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT COALESCE(SUM(token_estimate),0) FROM entries"
                              " WHERE session_id=?1 AND id>=?2 AND id<?3", -1, &s, NULL)
           == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_int64(s, 2, from);
    sqlite3_bind_int64(s, 3, to);
    assert(sqlite3_step(s) == SQLITE_ROW);
    int v = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    return v;
}

static int idx_of(const ContextPlan *p, int64_t id) {
    for (int i = 0; i < p->count; i++)
        if (p->entries[i].id == id) return i;
    assert(0 && "entry not on the plan");
    return -1;
}

/* Measured size of the plan's entries from id `from` up to (not including)
 * id `to` (0 = the end of the branch), in branch order. */
static int measured(sqlite3 *db, int64_t sid, int64_t from, int64_t to, int *head) {
    Config cfg = {0};
    cfg.context_window = 1000000;
    ContextPlan p = {0};
    assert(context_plan(db, sid, &cfg, 0, &p) == 0);
    int i = idx_of(&p, from), end = to ? idx_of(&p, to) : p.count, sum = 0;
    for (; i < end; i++) sum += p.entries[i].tokens;
    if (head) *head = p.head;
    context_plan_free(&p);
    return sum;
}

static int64_t new_session(sqlite3 *db) {
    return session_create(db, "s", "default", -1, 0);
}

/* A segment — one response, its call and result, up to the next report —
 * measures usage_in(next) − usage_in(this) when the two can be compared, and
 * never less than its estimate. */
static void test_segment_sizing(void) {
    sqlite3 *db = open_seeded();

    /* Reports on both sides, the provider counting twice the estimate. */
    int64_t sid = new_session(db);
    user_msg(db, sid, 300);
    int64_t r1 = respond(db, sid, "m", 5000, 0, 3000);
    int seg = est_ids(db, sid, r1, r1 + 3);              /* response, call, result */
    int64_t r2 = respond(db, sid, "m", 5000 + 2 * seg, 0, 0);
    assert(measured(db, sid, r1, r2, NULL) == 2 * seg);

    /* Measured below the estimate: the estimate stands. */
    sid = new_session(db);
    user_msg(db, sid, 300);
    r1 = respond(db, sid, "m", 5000, 0, 3000);
    int64_t probe = user_msg(db, sid, 30);
    seg = est_ids(db, sid, r1, probe + 1);
    r2 = respond(db, sid, "m", 5000 + seg / 2, 0, 0);
    assert(measured(db, sid, r1, r2, NULL) == seg);

    /* A response with no report: the segment runs on to the next report. */
    sid = new_session(db);
    user_msg(db, sid, 300);
    r1 = respond(db, sid, "m", 5000, 0, 3000);
    respond(db, sid, "m", 0, 0, 3000);
    int64_t r3 = respond(db, sid, "m", 5000 + 9000, 0, 0);
    assert(measured(db, sid, r1, r3, NULL) == 9000);

    /* A model switch between the reports: another tokenizer, so the
     * difference measures nothing and the segment is estimated — usage_out
     * counts only for the newest response, whose segment no report closes. */
    sid = new_session(db);
    user_msg(db, sid, 300);
    r1 = respond(db, sid, "m", 5000, 500, 3000);
    probe = respond(db, sid, "other", 20000, 0, 0);
    seg = est_ids(db, sid, r1, probe);
    assert(measured(db, sid, r1, probe, NULL) == seg);

    /* An errored response between them is never sent and is no report: the
     * difference across it still measures the segment. */
    sid = new_session(db);
    user_msg(db, sid, 300);
    r1 = respond(db, sid, "m", 5000, 0, 3000);
    Message err = { .role = ROLE_ASSISTANT, .content = "error: boom", .usage_in = 99999,
                    .stop_reason = STOP_REASON_ERROR };
    entry_append_with_iteration(db, sid, &err, 0);
    r2 = respond(db, sid, "m", 5000 + 6000, 0, 0);
    assert(measured(db, sid, r1, r2, NULL) == 6000);

    /* A compaction between the two requests: the second one was sent a
     * summary in place of the history the first one carried. */
    sid = new_session(db);
    int64_t u0 = user_msg(db, sid, 300);
    respond(db, sid, "m", 5000, 0, 0);
    int64_t u1 = user_msg(db, sid, 300);
    r1 = respond(db, sid, "m", 7000, 0, 3000);
    seg = est_ids(db, sid, r1, r1 + 3);
    assert(entry_compact(db, sid, u0, u1, "summary") > 0);
    probe = user_msg(db, sid, 30);
    seg += est_ids(db, sid, probe, probe + 1);
    r2 = respond(db, sid, "m", 1000, 0, 0);
    assert(measured(db, sid, r1, r2, NULL) == seg);
    exec_id(db, "UPDATE entries SET usage_in=7000+20000 WHERE id=?", r2);
    assert(measured(db, sid, r1, r2, NULL) == seg);       /* even when larger */
    db_close(db);
    printf("  PASS test_segment_sizing\n");
}

/* A turn's size is the sum of its segments: three iterations measured at
 * the provider's count, the last one's results (not yet sent) estimated,
 * the opening user message estimated. The head is the first report less
 * what it was sent with. */
static void test_turn_size_over_iterations(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db);
    int64_t u = user_msg(db, sid, 900);                   /* 304 */
    int64_t r1 = respond(db, sid, "m", 2000 + 304, 0, 3000);
    int64_t r2 = respond(db, sid, "m", 2000 + 304 + 4000, 0, 3000);
    int64_t r3 = respond(db, sid, "m", 2000 + 304 + 4000 + 5000, 0, 3000);
    int tail = est_ids(db, sid, r3, r3 + 10);
    int head = 0;
    assert(measured(db, sid, u, 0, &head) == 304 + 4000 + 5000 + tail);
    assert(head == 2000);
    assert(measured(db, sid, r1, r2, NULL) == 4000);
    /* usage_out on the newest response counts what the model wrote. */
    int out_est = est_ids(db, sid, r3, r3 + 2);
    exec_id(db, "UPDATE entries SET usage_out=900 WHERE id=?", r3);
    assert(measured(db, sid, r3, 0, NULL) == tail - out_est + 900);
    db_close(db);
    printf("  PASS test_turn_size_over_iterations\n");
}

/* Six turns of ~1008 estimated tokens fit a 7200-token budget; once the
 * provider has counted every turn at twice that, they do not. */
static void test_cut_keeps_fewer_turns(void) {
    sqlite3 *db = open_seeded();
    Config cfg = {0};
    cfg.context_window = 12000;                          /* budget 7200 */
    for (int reported = 0; reported <= 1; reported++) {
        int64_t sid = new_session(db);
        int pos = 0;
        for (int i = 0; i < 6; i++) {
            int64_t u = user_msg(db, sid, 3000);         /* 1004 */
            pos += 2 * est_ids(db, sid, u, u + 1) + (i ? 2 * 4 : 0);   /* + "ok" */
            respond(db, sid, "m", reported ? pos : 0, 0, 0);
        }
        ContextPlan plan = {0};
        assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
        assert(plan.count == 12);
        if (!reported) {
            assert(plan.cut == 0 && plan.head == 0);
        } else {
            /* Turn 1's message was counted at 2x, estimated at 1x: the
             * head carries the difference (1004). Turns 1-5 measure 2016,
             * turn 6 (not yet followed by a report) 1008: 7200 - 1004 holds
             * turns 6 to 4 and the response of turn 3, so the cut snaps
             * back to keep turn 3 whole — four of six turns. */
            assert(plan.head == 1004);
            assert(plan.cut == 4);
        }
        context_plan_free(&plan);
    }
    db_close(db);
    printf("  PASS test_cut_keeps_fewer_turns\n");
}

/* The probe holds the turn's measured size against the window: reported
 * segments at the provider's count, the results since the newest request
 * by estimate. */
static void test_probe_trips_earlier(void) {
    sqlite3 *db = open_seeded();
    for (int reported = 0; reported <= 1; reported++) {
        int64_t sid = new_session(db);
        int64_t u = user_msg(db, sid, 7000);
        int ask = est_ids(db, sid, u, u + 1);
        int64_t r1 = respond(db, sid, "m", reported ? ask : 0, 0, 12000);
        int seg = est_ids(db, sid, r1, r1 + 3);
        int64_t r2 = respond(db, sid, "m", reported ? ask + 2 * seg : 0, 0, 6000);
        int tail = est_ids(db, sid, r2, r2 + 3);         /* never sent yet */
        int turn = 0;
        int over = context_turn_overflows(db, sid, 10000, 0, &turn);
        if (!reported) {
            assert(turn == ask + seg + tail && over == 0);
        } else {
            assert(turn == ask + 2 * seg + tail && over == 1);
        }
    }
    db_close(db);
    printf("  PASS test_probe_trips_earlier\n");
}

/* What the trigger measured before the provider's count was the measure
 * (pre-05a69dc): max(branch estimate + head, newest usage_in, sent / 3)
 * against the limit less the last turn's estimate. */
static int old_trigger_margin(sqlite3 *db, int64_t sid, int head, int64_t newest_it,
                              int newest_in, int *last_turn) {
    int branch = (int)db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=?", sid, 0);
    *last_turn = (int)db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=?1 AND turn_id="
        "(SELECT MAX(turn_id) FROM entries WHERE session_id=?1)", sid, 0);
    int sent = (int)db_scalar_i64(db,
        "SELECT COALESCE(MAX(length(json(request_body))),0)/3 FROM llm_responses"
        " WHERE iteration_id=?", newest_it, 0);
    int size = branch + head;
    if (newest_in > size) size = newest_in;
    if (sent > size) size = sent;
    return size + *last_turn;                            /* fires when > limit */
}

static void set_window_just_under(Config *cfg, int need) {
    /* limit = 0.6 x window must be < need for the old trigger to fire */
    cfg->context_window = (int)((float)(need - 1) / 0.6f);
    while ((int)(0.6f * (float)cfg->context_window) >= need) cfg->context_window--;
}

/* Without a turn edit the trigger reads no lower than before: whichever of
 * the old measures dominated — the estimate, the provider's count, the
 * sent body — a limit the old trigger fired at, the new one fires at too. */
static void test_trigger_not_lower_without_edit(void) {
    for (int which = 0; which < 3; which++) {
        sqlite3 *db = open_seeded();
        int64_t sid = new_session(db);
        Config cfg = {0};
        cfg.compaction = 1;
        int head = context_head_estimate(db, "default", sid, &cfg, NULL);
        user_msg(db, sid, 9000);
        int64_t r1 = respond(db, sid, "m", which == 1 ? 3 * 5000 : 0, 0, 6000);
        user_msg(db, sid, 600);
        int64_t r2 = respond(db, sid, "m", which == 1 ? 3 * 5000 + 9000 : 0, 0, 0);
        int64_t it = db_scalar_i64(db, "SELECT iteration_id FROM entries WHERE id=?", r2, 0);
        if (which == 2) add_body(db, sid, it, 3 * 40000);
        (void)r1;
        int last = 0;
        int need = old_trigger_margin(db, sid, head, it, which == 1 ? 3 * 5000 + 9000 : 0, &last);
        set_window_just_under(&cfg, need);
        assert(session_needs_compaction(db, sid, &cfg) == 1);
        db_close(db);
    }
    printf("  PASS test_trigger_not_lower_without_edit\n");
}

/* A turn edit that drops the turn's big tool output: the trigger that
 * follows sees the shrink, though the newest provider count predates it. */
static void test_trigger_sees_turn_edit(void) {
    sqlite3 *db = open_seeded();
    assert(sqlite3_exec(db, "INSERT INTO tools(name, droppable) VALUES ('look',1);",
                        NULL, NULL, NULL) == SQLITE_OK);
    int64_t sid = new_session(db);
    Config cfg = {0};
    cfg.compaction = 1;
    int head = context_head_estimate(db, "default", sid, &cfg, NULL);

    user_msg(db, sid, 30);
    respond(db, sid, "m", head + 20, 0, 0);
    int64_t u = user_msg(db, sid, 30);
    int64_t r1 = respond(db, sid, "m", 0, 0, 0);
    /* The droppable call, as the edit expects it: a tool_calls row too. */
    int64_t it1 = db_scalar_i64(db, "SELECT iteration_id FROM entries WHERE id=?", r1, 0);
    int64_t c1 = entry_append_typed(db, sid, it1, "tool_call", 1, "{}", "c1", "look", 0,
                                    STOP_REASON_NONE, NULL, 0, 0, 0);
    exec_id(db, "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
                " SELECT session_id, id, 'c1', 'look', 'done' FROM entries WHERE id=?", c1);
    char *out = filler(24000);                           /* ~8000 estimated */
    entry_append_typed(db, sid, it1, "tool_result", 2, out, "c1", "look", 0,
                       STOP_REASON_NONE, NULL, 0, 0, 0);
    free(out);
    /* The provider counted the turn's output at twice the estimate. */
    int before = est_ids(db, sid, u, c1 + 2);
    int64_t fin = respond(db, sid, "m", head + 20 + 2 * before, 0, 0);
    assert(fin > 0);

    /* Limit sits between the measured size (~16k) and what is left after
     * the edit; the estimate alone (~8k) was always under it. */
    cfg.context_window = (int)((head + 12000) / 0.6f);
    assert(session_needs_compaction(db, sid, &cfg) == 1);

    char *err = NULL;
    assert(turn_edit_apply(db, sid, "{\"edits\":[{\"op\":\"drop\",\"call_id\":\"c1\"}]}",
                           "hook:t", &err) == TURN_EDIT_APPLIED);
    assert(session_needs_compaction(db, sid, &cfg) == 0);
    db_close(db);
    printf("  PASS test_trigger_sees_turn_edit\n");
}

/* After a turn edit the reports the edit invalidated are passed over: the
 * edited turn is estimated until the next request, which measures it again
 * against the report that opened it. */
static void test_edited_turn_remeasured(void) {
    sqlite3 *db = open_seeded();
    assert(sqlite3_exec(db, "INSERT INTO tools(name, droppable) VALUES ('look',1);",
                        NULL, NULL, NULL) == SQLITE_OK);
    int64_t sid = new_session(db);
    int64_t u = user_msg(db, sid, 300);
    int64_t r1 = respond(db, sid, "m", 2000, 0, 0);
    int64_t it1 = db_scalar_i64(db, "SELECT iteration_id FROM entries WHERE id=?", r1, 0);
    int64_t c1 = entry_append_typed(db, sid, it1, "tool_call", 1, "{}", "c1", "look", 0,
                                    STOP_REASON_NONE, NULL, 0, 0, 0);
    exec_id(db, "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
                " SELECT session_id, id, 'c1', 'look', 'done' FROM entries WHERE id=?", c1);
    /* A second call that stays keeps the iteration (and r1) on the branch. */
    entry_append_typed(db, sid, it1, "tool_call", 2, "{}", "c2", "look", 0,
                       STOP_REASON_NONE, NULL, 0, 0, 0);
    char *out = filler(24000);
    entry_append_typed(db, sid, it1, "tool_result", 3, out, "c1", "look", 0,
                       STOP_REASON_NONE, NULL, 0, 0, 0);
    free(out);
    int64_t res2 = entry_append_typed(db, sid, it1, "tool_result", 4, "small", "c2", "look",
                                      0, STOP_REASON_NONE, NULL, 0, 0, 0);
    int seg = est_ids(db, sid, r1, res2 + 1);
    respond(db, sid, "m", 2000 + 2 * seg, 0, 0);          /* the answer */
    assert(measured(db, sid, u, 0, NULL) > 2 * seg);

    char *err = NULL;
    assert(turn_edit_apply(db, sid, "{\"edits\":[{\"op\":\"drop\",\"call_id\":\"c1\"}]}",
                           "hook:t", &err) == TURN_EDIT_APPLIED);
    int left = measured(db, sid, u, 0, NULL);             /* estimate: no report since */
    int est = (int)db_scalar_i64(db,
        "WITH RECURSIVE b(id) AS (SELECT leaf_id FROM sessions WHERE id=?1"
        " UNION ALL SELECT e.parent_id FROM entries e JOIN b ON e.id=b.id WHERE e.parent_id>0)"
        " SELECT SUM(token_estimate) FROM entries WHERE id IN (SELECT id FROM b)", sid, 0);
    assert(left == est && left < 200);

    /* The next turn's request counts the edited turn at 3x what is left. */
    int64_t u2 = user_msg(db, sid, 30);
    int after = left - est_ids(db, sid, u, u + 1) + est_ids(db, sid, u2, u2 + 1);
    int64_t r3 = respond(db, sid, "m", 2000 + 3 * after, 0, 0);
    assert(measured(db, sid, r1, r3, NULL) == 3 * after);
    db_close(db);
    printf("  PASS test_edited_turn_remeasured\n");
}

/* No report anywhere: every size is the estimate and the head the estimate
 * passed in — the cut, the probe and the trigger read as they always have. */
static void test_no_reports_unchanged(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db);
    for (int i = 0; i < 5; i++) {
        user_msg(db, sid, 2000);
        respond(db, sid, NULL, 0, 0, 3000);
        respond(db, sid, NULL, 0, 0, 0);
    }
    Config cfg = {0};
    cfg.context_window = 8000;
    ContextPlan p = {0};
    assert(context_plan(db, sid, &cfg, 700, &p) == 0);
    int total = 0;
    for (int i = 0; i < p.count; i++) {
        assert(p.entries[i].tokens == p.entries[i].token_estimate);
        total += p.entries[i].tokens;
    }
    assert(p.head == 700);
    context_plan_free(&p);
    int est = (int)db_scalar_i64(db, "SELECT SUM(token_estimate) FROM entries"
                                     " WHERE session_id=?", sid, 0);
    assert(total == est);
    int last = (int)db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=?1 AND turn_id="
        "(SELECT MAX(turn_id) FROM entries WHERE session_id=?1)", sid, 0);
    int turn = 0;
    assert(context_turn_overflows(db, sid, last + 700, 700, &turn) == 0 && turn == last);
    assert(context_turn_overflows(db, sid, last + 699, 700, &turn) == 1);
    db_close(db);
    printf("  PASS test_no_reports_unchanged\n");
}

int main(void) {
    TEST_INIT();
    printf("test_context_accounting:\n");
    test_large_head_triggers();
    test_provider_floor_and_staleness();
    test_sent_payload_floor();
    test_estimate_divisor();
    test_segment_sizing();
    test_turn_size_over_iterations();
    test_cut_keeps_fewer_turns();
    test_probe_trips_earlier();
    test_trigger_not_lower_without_edit();
    test_trigger_sees_turn_edit();
    test_edited_turn_remeasured();
    test_no_reports_unchanged();
    printf("All context accounting tests passed.\n");
    return 0;
}
