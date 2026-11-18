/* Context accounting: the size the trim reads must see the whole request —
 * head (system prompt + tools) and the provider's measures — not just the
 * entries' own estimates; rescaled estimates must err high (chars/3); and an
 * entry's size is the provider's count, stored once when a response reports
 * it (db_response.c, usage_attribute), its estimate until then. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "db.h"
#include "db_response.h"
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

/* The session's size as the trim reads it: head (measured when a usable
 * report says so, else the estimate given) plus every entry it would send. */
static int size_of(sqlite3 *db, int64_t sid, int head_estimate, int *head_out) {
    ContextPlan m = {0};
    assert(context_measure(db, sid, head_estimate, &m) == 0);
    int t = m.head;
    for (int i = 0; i < m.count; i++) t += m.entries[i].tokens;
    if (head_out) *head_out = m.head;
    context_plan_free(&m);
    return t;
}

/* Small entries, huge head: a size that counts entries only never sees it,
 * and the provider 400s with the head alone over budget. The trim counts it. */
static void test_large_head_counts(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.context_window = 10000;
    int64_t it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    ContextLimits lim = { .window = 10000, .threshold = 6000, .target = 3000 };
    TrimReport r;
    int head = context_head_estimate(db, "default", sid, &cfg, "");
    assert(turn_edit_trim(db, sid, head, &lim, &r) == TRIM_UNDER);

    add_tool(db, "fat", 30000);      /* ~10000 head tokens */
    head = context_head_estimate(db, "default", sid, &cfg, "");
    assert(head >= 10000);
    assert(size_of(db, sid, head, NULL) > 6000);
    assert(turn_edit_trim(db, sid, head, &lim, &r) == TRIM_OVER);
    db_close(db);
    printf("  PASS test_large_head_counts\n");
}

/* The provider's prompt_tokens measures the head when fresh — it lifts it
 * above our estimate — and is ignored once a compaction post-dates it: that
 * request was sent entries the branch no longer has. */
static void test_provider_floor_and_staleness(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    int64_t it = db_next_iteration_id(db, sid);
    int64_t e1 = append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    it = db_next_iteration_id(db, sid);
    int64_t e3 = append(db, sid, ROLE_USER, "more", 0, it);
    append(db, sid, ROLE_ASSISTANT, "sure", 9000, it);   /* provider says 9000 */
    int head = 0;
    assert(size_of(db, sid, 0, &head) > 8900 && head > 8900);

    /* Compact away the middle: the 9000 figure predates the summary. */
    assert(test_legacy_compact(db, sid, e1, e3, "summary") > 0);
    assert(size_of(db, sid, 0, &head) < 100 && head == 0);

    /* A new post-compaction response reinstates the provider term. */
    it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "again", 0, it);
    append(db, sid, ROLE_ASSISTANT, "ok", 9500, it);
    assert(size_of(db, sid, 0, &head) > 9400 && head > 9300);
    db_close(db);
    printf("  PASS test_provider_floor_and_staleness\n");
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


/* ── Stored sizes ─────────────────────────────────────────────────── */

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

static int tok(sqlite3 *db, int64_t id) {
    return (int)db_scalar_i64(db, "SELECT COALESCE(tokens,-1) FROM entries WHERE id=?", id, -2);
}

static void set_tok(sqlite3 *db, int64_t id, int tokens) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "UPDATE entries SET tokens=?2 WHERE id=?1", -1, &s, NULL)
           == SQLITE_OK);
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_int(s, 2, tokens);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

/* One response through the real ingest, OpenAI-compatible wire: `in`/`out`
 * as usage, `think` reasoning tokens inside `out` (with a reasoning_details
 * blob to replay when > 0), `calls` tool calls. Returns the message's entry
 * id; the calls follow it at +1.., a reasoning entry precedes it at -1. */
static int64_t ingest_as(sqlite3 *db, int64_t sid, const char *model, int in, int out,
                         int think, int calls, int64_t *it_out) {
    char body[2048], tc[1024] = "", rs[256] = "";
    int64_t it = db_next_iteration_id(db, sid);
    for (int i = 0, n = 0; i < calls; i++)
        n += snprintf(tc + n, sizeof(tc) - (size_t)n,
                      "%s{\"id\":\"c%lld_%d\",\"type\":\"function\",\"function\":"
                      "{\"name\":\"look\",\"arguments\":\"{\\\"q\\\":\\\"somewhere\\\"}\"}}",
                      i ? "," : "", (long long)it, i);
    if (think > 0)
        snprintf(rs, sizeof(rs), ",\"reasoning_details\":[{\"type\":\"reasoning.encrypted\","
                                 "\"data\":\"b3BhcXVl\"}]");
    snprintf(body, sizeof(body),
             "{\"choices\":[{\"finish_reason\":\"%s\",\"message\":{\"role\":\"assistant\","
             "\"content\":\"a reply of some ordinary length, forty-odd bytes\"%s%s%s%s}}],"
             "\"usage\":{\"prompt_tokens\":%d,\"completion_tokens\":%d,"
             "\"completion_tokens_details\":{\"reasoning_tokens\":%d}}}",
             calls ? "tool_calls" : "stop", rs, calls ? ",\"tool_calls\":[" : "", tc,
             calls ? "]" : "", in, out, think);
    TypedIngestResult ir;
    assert(db_ingest_response(db, sid, it, model, ENDPOINT_OPENAI, body, NULL, 0, &ir)
           == LLM_RESP_OK);
    if (it_out) *it_out = it;
    return ir.assistant_entry_id;
}

static int64_t ingest(sqlite3 *db, int64_t sid, int in, int out, int think, int calls,
                      int64_t *it_out) {
    return ingest_as(db, sid, "m", in, out, think, calls, it_out);
}

/* The result answering call `k` of iteration `it`. */
static int64_t result(sqlite3 *db, int64_t sid, int64_t it, int k, int bytes) {
    char cid[48];
    snprintf(cid, sizeof(cid), "c%lld_%d", (long long)it, k);
    char *t = filler(bytes);
    int64_t id = entry_append_typed(db, sid, it, "tool_result", 10 + k, t, cid, "look", 0,
                                    STOP_REASON_NONE, NULL, 0, 0, 0);
    free(t);
    return id;
}

static int ctx_tokens(sqlite3 *db, int64_t sid) {
    return (int)db_scalar_i64(db, "SELECT COALESCE(turn_context_tokens,-1) FROM sessions"
                                  " WHERE id=?", sid, -2);
}

/* A response's own entries share its usage_out: the reasoning entry what
 * the provider says the reasoning was, the message and each call the rest
 * by length, and the parts add up to the whole. */
static void test_output_shared(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db);
    user_msg(db, sid, 300);
    int64_t a = ingest(db, sid, 2000, 90, 30, 2, NULL);
    assert(tok(db, a - 1) == 30);                        /* reasoning */
    assert(tok(db, a) > 0 && tok(db, a + 1) > 0 && tok(db, a + 2) > 0);
    assert(tok(db, a) + tok(db, a + 1) + tok(db, a + 2) == 60);
    assert(tok(db, a + 1) == tok(db, a + 2));            /* the same call twice */
    /* No usage reported: nothing is measured. */
    int64_t b = ingest(db, sid, 0, 0, 0, 0, NULL);
    assert(tok(db, b) == -1);
    db_close(db);
    printf("  PASS test_output_shared\n");
}

/* What was appended between two reports shares the growth in usage_in that
 * the earlier response does not account for — here two results, by length.
 * The message that opened the session has no report before it: unmeasured. */
static void test_appended_shared(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db);
    int64_t u = user_msg(db, sid, 300), it;
    int64_t a = ingest(db, sid, 2000, 60, 0, 2, &it);
    int64_t r0 = result(db, sid, it, 0, 3000), r1 = result(db, sid, it, 1, 6000);
    assert(tok(db, r0) == -1);
    ingest(db, sid, 2000 + 60 + 2700, 20, 0, 0, NULL);
    assert(tok(db, u) == -1);
    assert(tok(db, r0) + tok(db, r1) == 2700);
    assert(tok(db, r0) > 850 && tok(db, r0) < 950);      /* a third of it */
    assert(tok(db, a) + tok(db, a + 1) + tok(db, a + 2) == 60);   /* untouched */

    /* A response that stopped on an error is never sent: it and what follows
     * it take no share. */
    int64_t u2 = user_msg(db, sid, 300);
    Message err = { .role = ROLE_ASSISTANT, .content = "error: boom",
                    .stop_reason = STOP_REASON_ERROR };
    int64_t e = entry_append_with_iteration(db, sid, &err, 0);
    ingest(db, sid, 2000 + 60 + 2700 + 20 + 95, 20, 0, 0, NULL);
    assert(tok(db, u2) == 95 && tok(db, e) == -1);
    db_close(db);
    printf("  PASS test_appended_shared\n");
}

/* Inside a turn the previous response's reasoning is replayed and counted;
 * when the growth only fits without it, the provider did not count it and
 * the entry reads 0 from then on. */
static void test_reasoning_within_turn(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db), it;
    user_msg(db, sid, 300);
    int64_t a = ingest(db, sid, 2000, 840, 800, 1, &it);
    int64_t r = result(db, sid, it, 0, 3000);
    ingest(db, sid, 2000 + 840 + 900, 20, 0, 0, NULL);
    assert(tok(db, a - 1) == 800 && tok(db, r) == 900);

    sid = new_session(db);
    user_msg(db, sid, 300);
    a = ingest(db, sid, 2000, 840, 800, 1, &it);
    r = result(db, sid, it, 0, 3000);
    ingest(db, sid, 2000 + 40 + 900, 20, 0, 0, NULL);
    assert(tok(db, a - 1) == 0 && tok(db, r) == 900);
    db_close(db);
    printf("  PASS test_reasoning_within_turn\n");
}

/* Across a turn boundary the user context block was rebuilt: the new block
 * shares the growth with the messages that opened the turn, and the old
 * block's count is given back — so a block that grows is not booked on any
 * entry, and one that shrinks takes nothing from them. */
static void test_context_block_at_boundary(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db);
    char *block = filler(3000);
    block[600] = 0;
    session_set_turn_context(db, sid, block);            /* estimate 200 */
    user_msg(db, sid, 300);
    ingest(db, sid, 2000, 50, 0, 0, NULL);
    assert(ctx_tokens(db, sid) == 200);                  /* no report before: estimated */

    block[600] = 'w';
    session_set_turn_context(db, sid, block);            /* estimate 1000 */
    int64_t u2 = user_msg(db, sid, 300);                 /* estimate 104 */
    /* really 80 for the message and 900 for the block, 200 given back */
    ingest(db, sid, 2000 + 50 + 80 + 900 - 200, 50, 0, 0, NULL);
    assert(tok(db, u2) + ctx_tokens(db, sid) == 980);
    assert(ctx_tokens(db, sid) == 980 * 1000 / 1104);
    int big = ctx_tokens(db, sid), in = 2000 + 50 + 80 + 900 - 200;

    block[600] = 0;
    session_set_turn_context(db, sid, block);            /* back to 200 */
    int64_t u3 = user_msg(db, sid, 300);
    ingest(db, sid, in + 50 + 80 + 180 - big, 50, 0, 0, NULL);
    assert(tok(db, u3) + ctx_tokens(db, sid) == 260);
    assert(tok(db, u3) > 60 && tok(db, u3) < 120);
    assert(tok(db, u2) == 980 - big);                    /* as it was measured */
    free(block);
    db_close(db);
    printf("  PASS test_context_block_at_boundary\n");
}

/* A provider that is sent the previous turn's reasoning and counts it keeps
 * it on the books; one that does not count it past the boundary has the
 * whole turn's reasoning set to 0, and the opening message still measured. */
static void test_reasoning_across_boundary(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db), it;
    user_msg(db, sid, 300);
    int64_t a = ingest(db, sid, 2000, 832, 793, 0, NULL);
    int64_t u = user_msg(db, sid, 300);
    ingest(db, sid, 2000 + 832 + 90, 20, 0, 0, NULL);
    assert(tok(db, a - 1) == 793 && tok(db, u) == 90);

    sid = new_session(db);
    user_msg(db, sid, 300);
    a = ingest(db, sid, 2000, 340, 300, 1, &it);
    int64_t r = result(db, sid, it, 0, 3000);
    int64_t b = ingest(db, sid, 2000 + 340 + 900, 700, 660, 0, NULL);
    assert(tok(db, a - 1) == 300 && tok(db, b - 1) == 660);
    u = user_msg(db, sid, 300);
    /* neither response's reasoning is in this request's count */
    ingest(db, sid, 2000 + 340 + 900 - 300 + 40 + 90, 20, 0, 0, NULL);
    assert(tok(db, a - 1) == 0 && tok(db, b - 1) == 0);
    assert(tok(db, u) == 90 && tok(db, r) == 900);
    db_close(db);
    printf("  PASS test_reasoning_across_boundary\n");
}

/* A difference is only trusted when the two requests can be compared: not
 * across a model switch (whose reasoning is stripped, and reads 0), not
 * across a compaction, not when what is left is nowhere near the size of
 * what was appended. Then the entries keep their estimate. */
static void test_untrusted_difference(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db), it;
    user_msg(db, sid, 300);
    int64_t a = ingest(db, sid, 2000, 540, 500, 1, &it);
    int64_t r = result(db, sid, it, 0, 3000);
    int64_t b = ingest_as(db, sid, "other", 9000, 30, 0, 0, NULL);
    assert(tok(db, r) == -1 && tok(db, a - 1) == 0);
    assert(tok(db, b) == 30);                            /* its own output still is */

    sid = new_session(db);
    int64_t u0 = user_msg(db, sid, 300);
    ingest(db, sid, 2000, 50, 0, 0, NULL);
    int64_t u1 = user_msg(db, sid, 300);
    ingest(db, sid, 2000 + 50 + 90, 50, 0, 0, NULL);
    assert(tok(db, u1) == 90);
    assert(test_legacy_compact(db, sid, u0, u1, "summary") > 0);
    int64_t u2 = user_msg(db, sid, 300);
    ingest(db, sid, 900, 50, 0, 0, NULL);
    assert(tok(db, u2) == -1);

    sid = new_session(db);
    user_msg(db, sid, 300);
    ingest(db, sid, 2000, 50, 0, 0, NULL);
    u1 = user_msg(db, sid, 300);
    ingest(db, sid, 2000 + 50 + 40000, 50, 0, 0, NULL);  /* the head changed under it */
    assert(tok(db, u1) == -1);
    u2 = user_msg(db, sid, 300);
    ingest(db, sid, 2000 + 50 + 40000 + 50 + 90, 50, 0, 0, NULL);
    assert(tok(db, u2) == 90);                           /* and the next one is fine */
    db_close(db);
    printf("  PASS test_untrusted_difference\n");
}

/* The plan reads each entry at its stored count and its estimate until it
 * has one, and the head is the newest report less what it was sent with. */
static void test_plan_reads_stored(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = new_session(db), it;
    int64_t u = user_msg(db, sid, 900);                  /* 304 */
    int64_t a = ingest(db, sid, 2000 + 304, 440, 400, 1, &it);
    int64_t r = result(db, sid, it, 0, 3000);
    int64_t b = ingest(db, sid, 2000 + 304 + 440 + 900, 30, 0, 1, &it);
    int64_t r2 = result(db, sid, it, 0, 3000);           /* not yet sent: 1004 */
    int head = 0;
    assert(measured(db, sid, u, 0, &head) == 304 + 440 + 900 + 30 + 1004);
    assert(head == 2000);
    assert(measured(db, sid, a - 1, a, NULL) == 400);    /* reasoning, replayed */
    assert(measured(db, sid, r, b, NULL) == 900);
    assert(size_of(db, sid, 0, NULL) == 2000 + 304 + 440 + 900 + 30 + 1004);
    (void)r2;
    db_close(db);
    printf("  PASS test_plan_reads_stored\n");
}

/* Six turns fit a 7200-token budget by estimate; once each turn's result
 * has been measured at twice its estimate, three do — and the cut, landing
 * inside the fourth, snaps back to keep that one whole. */
static void test_cut_keeps_fewer_turns(void) {
    sqlite3 *db = open_seeded();
    Config cfg = {0};
    cfg.context_window = 14400;                          /* budget: half, 7200 */
    for (int reported = 0; reported <= 1; reported++) {
        int64_t sid = new_session(db);
        int turn = 0;
        for (int i = 0; i < 6; i++) {
            int64_t u = user_msg(db, sid, 300);
            int64_t r1 = respond(db, sid, "m", 0, 0, 3000);
            int64_t r2 = respond(db, sid, "m", 0, 0, 0);
            int res = est_ids(db, sid, r1 + 2, r1 + 3);
            if (reported) set_tok(db, r1 + 2, 2 * res);
            turn = est_ids(db, sid, u, r2 + 1) + (reported ? res : 0);
        }
        if (reported) assert(3 * turn <= 7200 && 4 * turn > 7200);
        else assert(6 * turn <= 7200);
        ContextPlan plan = {0};
        assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
        assert(plan.count == 30 && plan.head == 0);
        assert(plan.cut == (reported ? 10 : 0));
        context_plan_free(&plan);
    }
    db_close(db);
    printf("  PASS test_cut_keeps_fewer_turns\n");
}

/* A turn edit that drops the turn's big tool output takes its stored count
 * with it: the size the trim reads sees the shrink at once, with no request
 * in between. A replaced result keeps its measure, scaled. */
static void test_edit_changes_sizes(void) {
    sqlite3 *db = open_seeded();
    assert(sqlite3_exec(db, "INSERT INTO tools(name, droppable) VALUES ('look',1);",
                        NULL, NULL, NULL) == SQLITE_OK);
    int64_t sid = new_session(db), it;
    Config cfg = {0};
    int head = context_head_estimate(db, "default", sid, &cfg, NULL);

    user_msg(db, sid, 30);
    ingest(db, sid, head + 20, 20, 0, 0, NULL);
    int64_t u = user_msg(db, sid, 30);
    int64_t a = ingest(db, sid, head + 20 + 20 + 15, 40, 0, 2, &it);
    exec_id(db, "UPDATE tool_calls SET status='done' WHERE session_id=?", sid);
    int64_t big = result(db, sid, it, 0, 24000), small = result(db, sid, it, 1, 3000);
    /* the provider counts the big result at twice its estimate */
    ingest(db, sid, head + 20 + 20 + 15 + 40 + 16000 + 1000, 30, 0, 0, NULL);
    assert(tok(db, big) + tok(db, small) == 17000 && tok(db, big) > 15000);
    int was_small = tok(db, small);

    /* The measured size (~17k over the head) is over a line that what is
     * left after the drop is under; the estimate alone (~9k) always was. */
    assert(size_of(db, sid, head, NULL) > head + 13000);

    char edits[1024], *err = NULL;
    snprintf(edits, sizeof(edits),
             "{\"edits\":[{\"op\":\"drop\",\"call_id\":\"c%lld_0\"},"
             "{\"op\":\"replace\",\"call_id\":\"c%lld_1\",\"text\":\"%0600d\"}]}",
             (long long)it, (long long)it, 7);
    assert(turn_edit_apply(db, sid, edits, "hook:t", &err) == TURN_EDIT_APPLIED);
    assert(size_of(db, sid, head, NULL) < head + 13000);
    int64_t repl = db_scalar_i64(db, "SELECT id FROM entries WHERE session_id=?1"
                                     " AND type='tool_result' ORDER BY id DESC LIMIT 1", sid, 0);
    assert(repl > small && tok(db, repl) == was_small * 600 / 3000);
    /* What is left of the turn: the question, the message, one call, the
     * replacement, the answer. */
    assert(measured(db, sid, u, 0, NULL) < 15 + 40 + tok(db, repl) + 30 + 1);

    /* The next turn's first request is not compared with the edited turn's
     * last one: its opening message keeps its estimate. */
    int64_t u3 = user_msg(db, sid, 300);
    ingest(db, sid, head + 500, 30, 0, 0, NULL);
    assert(tok(db, u3) == -1);
    (void)a;
    db_close(db);
    printf("  PASS test_edit_changes_sizes\n");
}

/* No report anywhere: every size is the estimate and the head the estimate
 * passed in. */
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
        assert(p.entries[i].tokens == (int)db_scalar_i64(db,
                   "SELECT token_estimate FROM entries WHERE id=?", p.entries[i].id, -1));
        total += p.entries[i].tokens;
    }
    assert(p.head == 700);
    context_plan_free(&p);
    assert(size_of(db, sid, 700, NULL) == total + 700);
    db_close(db);
    printf("  PASS test_no_reports_unchanged\n");
}

int main(void) {
    TEST_INIT();
    printf("test_context_accounting:\n");
    test_large_head_counts();
    test_provider_floor_and_staleness();
    test_estimate_divisor();
    test_output_shared();
    test_appended_shared();
    test_reasoning_within_turn();
    test_context_block_at_boundary();
    test_reasoning_across_boundary();
    test_untrusted_difference();
    test_plan_reads_stored();
    test_cut_keeps_fewer_turns();
    test_edit_changes_sizes();
    test_no_reports_unchanged();
    printf("All context accounting tests passed.\n");
    return 0;
}
