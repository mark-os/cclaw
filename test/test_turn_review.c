#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "buf.h"
#include "config.h"
#include "context.h"
#include "llm_payload.h"
#include "turn_review.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DB_PATH = "/tmp/test_cclaw_turn_review.db";

/* ── Fixtures ──────────────────────────────────────────────────────*/

static sqlite3 *open_seeded(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    test_seed_agent(db, "default");
    /* Tool facts as tools_sync_to_db / a promoted manifest leave them:
     * two look-up tools, one whose call is its effect, one retrieving tool
     * that can also store a credential. */
    assert(sqlite3_exec(db,
        "INSERT INTO tools(name, droppable) VALUES"
        " ('look',1),('look2',1),('act',0),('fetch',1);", NULL, NULL, NULL) == SQLITE_OK);
    return db;
}

static void run(sqlite3 *db, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void run(sqlite3 *db, const char *fmt, ...) {
    char sql[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sql, sizeof(sql), fmt, ap);
    va_end(ap);
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "FAIL sql: %s\n  %s\n", err ? err : "?", sql);
        abort();
    }
}

/* Column 0 of a one-row query, as text (caller frees); NULL for SQL NULL. */
static char *q(sqlite3 *db, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static char *q(sqlite3 *db, const char *fmt, ...) {
    char sql[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sql, sizeof(sql), fmt, ap);
    va_end(ap);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "FAIL sql: %s\n  %s\n", sqlite3_errmsg(db), sql);
        abort();
    }
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        out = test_strdup_((const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static void expect_q(sqlite3 *db, const char *want, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void expect_q(sqlite3 *db, const char *want, const char *fmt, ...) {
    char sql[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sql, sizeof(sql), fmt, ap);
    va_end(ap);
    char *got = q(db, "%s", sql);
    if (!want ? got != NULL : (!got || strcmp(got, want) != 0)) {
        fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n", sql, got ? got : "(null)",
                want ? want : "(null)");
        abort();
    }
    free(got);
}

/* A one-value query over a JSON document bound as ?1 (it may be large and
 * may hold quotes, so it is never pasted into the SQL). */
static void expect_json(sqlite3 *db, const char *json, const char *expr, const char *want) {
    char sql[512];
    snprintf(sql, sizeof(sql), "SELECT %s", expr);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        fprintf(stderr, "FAIL sql: %s\n  %s\n", sqlite3_errmsg(db), sql);
        abort();
    }
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *got = (const char *)sqlite3_column_text(st, 0);
    if (!want ? got != NULL : (!got || strcmp(got, want) != 0)) {
        fprintf(stderr, "FAIL %s\n  got  %s\n  want %s\n  in   %s\n", expr,
                got ? got : "(null)", want ? want : "(null)", json);
        abort();
    }
    sqlite3_finalize(st);
}

/* json_extract(json, path) as text. */
static void expect_at(sqlite3 *db, const char *json, const char *path, const char *want) {
    char expr[256];
    snprintf(expr, sizeof(expr), "json_extract(?1,'%s')", path);
    expect_json(db, json, expr, want);
}

static void expect_id_at(sqlite3 *db, const char *json, const char *path, int64_t want) {
    char w[32];
    snprintf(w, sizeof(w), "%lld", (long long)want);
    expect_at(db, json, path, w);
}

static int64_t add_user(sqlite3 *db, int64_t sid, const char *text) {
    Message m = {.role = ROLE_USER, .content = (char *)text};
    int64_t id = entry_append_with_iteration(db, sid, &m, 0);
    assert(id > 0);
    return id;
}

static int64_t add_typed(sqlite3 *db, int64_t sid, int64_t it, const char *type,
                         const char *content, const char *call_id, const char *tool) {
    int64_t id = entry_append_typed(db, sid, it, type, 0, content, call_id, tool, 0,
                                    STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(id > 0);
    return id;
}

/* A reasoning entry whose replay blob says `why`, produced by model m@p. */
static int64_t add_reasoning(sqlite3 *db, int64_t sid, int64_t it, const char *why) {
    int64_t id = add_typed(db, sid, it, "reasoning", NULL, NULL, NULL);
    run(db, "UPDATE entries SET reasoning_meta=json_object('provider','openai',"
            "'model','m@p','format','reasoning_details',"
            "'blob',json_array(json_object('text','%s'))) WHERE id=%lld", why, (long long)id);
    return id;
}

/* A tool_call entry with its workflow row. */
static int64_t add_call(sqlite3 *db, int64_t sid, int64_t it, const char *call_id,
                        const char *tool, const char *args, const char *status) {
    int64_t id = add_typed(db, sid, it, "tool_call", args, call_id, tool);
    run(db, "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
            " VALUES(%lld,%lld,'%s','%s','%s')", (long long)sid, (long long)id, call_id,
        tool, status);
    return id;
}

/* Lines long enough that a marker line is cheaper than the lines it stands
 * for — a trim that would not shrink the result is not applied. */
#define PAD " ................................................................"
static const char R1_LONG[] =
    "line-01" PAD "\nline-02" PAD "\nline-03" PAD "\nline-04" PAD "\nline-05" PAD "\n"
    "line-06" PAD "\nline-07" PAD "\nline-08" PAD "\nline-09" PAD "\nline-10" PAD;
static const char R3_LONG[] = "a-1" PAD "\na-2" PAD "\na-3" PAD "\na-4" PAD "\na-5" PAD;
static const char R4_LONG[] = "w-1" PAD "\nw-2" PAD "\nw-3" PAD "\nw-4" PAD "\n";

typedef struct {
    int64_t sid, turn;
    int64_t u0, a0;                                  /* an earlier turn */
    int64_t u;
    int64_t it1, rs1, a1, c1, c2, c3, r2, r1, r3;    /* a parallel batch */
    int64_t it2, rs2, a2, c4, r4;
    int64_t it3, rs3, fin;                           /* the answer */
} T;

/* A finished turn: a three-call batch answered out of order, a second
 * iteration with one call, the answer. long_results gives the results lines
 * worth trimming. */
static T seed_turn(sqlite3 *db, const char *name, int long_results) {
    T t = {0};
    t.sid = session_create(db, name, "default", -1, 0);
    assert(t.sid > 0);
    t.u0 = add_user(db, t.sid, "hi");
    t.a0 = add_typed(db, t.sid, db_next_iteration_id(db, t.sid), "assistant_message",
                     "hello", NULL, NULL);

    t.u = add_user(db, t.sid, "go");
    t.it1 = db_next_iteration_id(db, t.sid);
    t.rs1 = add_reasoning(db, t.sid, t.it1, "why1");
    t.a1 = add_typed(db, t.sid, t.it1, "assistant_message", "one", NULL, NULL);
    t.c1 = add_call(db, t.sid, t.it1, "c1", "look", "{\"path\":\"/a\"}", "done");
    t.c2 = add_call(db, t.sid, t.it1, "c2", "look2", "{\"k\":2}", "done");
    t.c3 = add_call(db, t.sid, t.it1, "c3", "act", "{\"k\":3}", "done");
    t.r2 = add_typed(db, t.sid, t.it1, "tool_result", "r2", "c2", "look2");
    t.r1 = add_typed(db, t.sid, t.it1, "tool_result", long_results ? R1_LONG : "r1",
                     "c1", "look");
    t.r3 = add_typed(db, t.sid, t.it1, "tool_result",
                     long_results ? R3_LONG : "r3", "c3", "act");

    t.it2 = db_next_iteration_id(db, t.sid);
    t.rs2 = add_reasoning(db, t.sid, t.it2, "why2");
    t.a2 = add_typed(db, t.sid, t.it2, "assistant_message", "two", NULL, NULL);
    t.c4 = add_call(db, t.sid, t.it2, "c4", "look", "{}", "done");
    t.r4 = add_typed(db, t.sid, t.it2, "tool_result",
                     long_results ? R4_LONG : "r4", "c4", "look");

    t.it3 = db_next_iteration_id(db, t.sid);
    t.rs3 = add_reasoning(db, t.sid, t.it3, "why3");
    t.fin = add_typed(db, t.sid, t.it3, "assistant_message", "done", NULL, NULL);

    t.turn = db_scalar_i64(db, "SELECT turn_id FROM entries WHERE id=?", t.fin, -1);
    assert(t.turn == 2);
    assert(db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", t.sid, -1) == t.fin);
    return t;
}

/* Everything an apply could touch, as one string: every column of every
 * entry, the sessions row's leaf, the workflow rows, the search index. */
static char *dump(sqlite3 *db) {
    static const char *SQL[] = {
        "SELECT * FROM entries ORDER BY id",
        "SELECT id, leaf_id, state FROM sessions ORDER BY id",
        "SELECT * FROM tool_calls ORDER BY id",
        "SELECT rowid, content FROM entries_fts ORDER BY rowid",
    };
    Buf b = {0};
    for (size_t k = 0; k < sizeof(SQL) / sizeof(SQL[0]); k++) {
        sqlite3_stmt *st;
        assert(sqlite3_prepare_v2(db, SQL[k], -1, &st, NULL) == SQLITE_OK);
        while (sqlite3_step(st) == SQLITE_ROW) {
            for (int c = 0; c < sqlite3_column_count(st); c++) {
                const char *v = (const char *)sqlite3_column_text(st, c);
                buf_append_str(&b, v ? v : "\x01");
                buf_append_char(&b, '\x02');
            }
            buf_append_char(&b, '\n');
        }
        sqlite3_finalize(st);
    }
    char *out = buf_take(&b);
    assert(out);
    return out;
}

/* The branch from the leaf, root-ward, as "id,id,…" — and proof on the way
 * that it is one chain that ends at the root. */
static char *branch(sqlite3 *db, int64_t sid) {
    char *out = q(db,
        "WITH RECURSIVE b(id, parent_id, lvl) AS ("
        "  SELECT id, parent_id, 0 FROM entries"
        "   WHERE id=(SELECT leaf_id FROM sessions WHERE id=%lld)"
        "  UNION ALL SELECT e.id, e.parent_id, b.lvl+1 FROM entries e JOIN b"
        "    ON e.id=b.parent_id WHERE b.lvl < 1000)"
        " SELECT group_concat(id, ',' ORDER BY lvl DESC) FROM b", (long long)sid);
    assert(out);
    expect_q(db, "-1",
        "WITH RECURSIVE b(id, parent_id, lvl) AS ("
        "  SELECT id, parent_id, 0 FROM entries"
        "   WHERE id=(SELECT leaf_id FROM sessions WHERE id=%lld)"
        "  UNION ALL SELECT e.id, e.parent_id, b.lvl+1 FROM entries e JOIN b"
        "    ON e.id=b.parent_id WHERE b.lvl < 1000)"
        " SELECT parent_id FROM b ORDER BY lvl DESC LIMIT 1", (long long)sid);
    return out;
}

static void expect_branch(sqlite3 *db, int64_t sid, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void expect_branch(sqlite3 *db, int64_t sid, const char *fmt, ...) {
    char want[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(want, sizeof(want), fmt, ap);
    va_end(ap);
    char *got = branch(db, sid);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL branch:\n  got  %s\n  want %s\n", got, want);
        abort();
    }
    free(got);
}

static int64_t review_entry(sqlite3 *db, const T *t) {
    int64_t id = db_scalar_i64(db,
        "SELECT id FROM entries WHERE type='turn_review' AND session_id=?", t->sid, -1);
    assert(id > t->fin);
    return id;
}

/* ── Payload shape (as test_llm_payload.c) ─────────────────────────
 * Messages joined by '|', each "role:parts": a part is its text, "=<id>" for
 * a tool call (Gemini: the tool name), "<<id>" for a tool result, '*' for an
 * Anthropic cache breakpoint. */
static char *shape(sqlite3 *db, const char *body, EndpointType ep) {
    static const char *SQL[] = {
        [ENDPOINT_OPENAI] =
        "SELECT group_concat(json_extract(m.value,'$.role') || ':' || concat_ws(',',"
        "  json_extract(m.value,'$.content'),"
        "  (SELECT group_concat('=' || json_extract(c.value,'$.id'), ',')"
        "     FROM json_each(m.value,'$.tool_calls') c),"
        "  '<' || json_extract(m.value,'$.tool_call_id')), '|')"
        " FROM json_each(?1,'$.messages') m",
        [ENDPOINT_GEMINI] =
        "SELECT group_concat(json_extract(m.value,'$.role') || ':' ||"
        "  (SELECT group_concat(COALESCE(json_extract(p.value,'$.text'),"
        "     '=' || json_extract(p.value,'$.functionCall.name'),"
        "     '<' || json_extract(p.value,'$.functionResponse.name')), ',')"
        "     FROM json_each(m.value,'$.parts') p), '|')"
        " FROM json_each(?1,'$.contents') m",
        [ENDPOINT_ANTHROPIC] =
        "SELECT group_concat(json_extract(m.value,'$.role') || ':' ||"
        "  (SELECT group_concat(CASE json_extract(b.value,'$.type')"
        "       WHEN 'text' THEN json_extract(b.value,'$.text')"
        "       WHEN 'tool_use' THEN '=' || json_extract(b.value,'$.id')"
        "       WHEN 'tool_result' THEN '<' || json_extract(b.value,'$.tool_use_id')"
        "       ELSE json_extract(b.value,'$.type') END ||"
        "     CASE WHEN json_extract(b.value,'$.cache_control') IS NULL THEN ''"
        "          ELSE '*' END, ',')"
        "     FROM json_each(m.value,'$.content') b), '|')"
        " FROM json_each(?1,'$.messages') m",
    };
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, SQL[ep], -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, body, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *v = (const char *)sqlite3_column_text(st, 0);
    char *r = test_strdup_(v ? v : "");
    sqlite3_finalize(st);
    return r;
}

static void build(sqlite3 *db, int64_t sid, EndpointType ep, LlmPayload *p) {
    Config cfg = {0};
    cfg.provider.model = "m";
    cfg.provider.model_id = "m@p";
    cfg.provider.endpoint_type = ep;
    cfg.context_window = 128000;
    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, NULL, p) == 0);
    context_plan_free(&plan);
}

static void expect_shape(sqlite3 *db, int64_t sid, EndpointType ep, const char *want) {
    LlmPayload p;
    build(db, sid, ep, &p);
    char *got = shape(db, p.body, ep);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL shape (endpoint %d):\n  got  %s\n  want %s\n", (int)ep,
                got, want);
        abort();
    }
    free(got);
    llm_payload_release(&p);
}

/* json_extract(body, path) on a freshly built request, as text. */
static void expect_body(sqlite3 *db, int64_t sid, EndpointType ep, const char *path,
                        const char *want) {
    LlmPayload p;
    build(db, sid, ep, &p);
    sqlite3_stmt *st;
    char sql[160];
    snprintf(sql, sizeof(sql), "SELECT json_extract(?1,'%s')", path);
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, p.body, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *got = (const char *)sqlite3_column_text(st, 0);
    if (!want ? got != NULL : (!got || strcmp(got, want) != 0)) {
        fprintf(stderr, "FAIL %s (endpoint %d):\n  got  %s\n  want %s\n", path, (int)ep,
                got ? got : "(absent)", want ? want : "(absent)");
        abort();
    }
    sqlite3_finalize(st);
    llm_payload_release(&p);
}

/* Does the request body, on every format, contain / lack this text? */
static void expect_text(sqlite3 *db, int64_t sid, const char *text, int present) {
    static const EndpointType EP[] = {ENDPOINT_OPENAI, ENDPOINT_GEMINI, ENDPOINT_ANTHROPIC};
    for (int i = 0; i < 3; i++) {
        LlmPayload p;
        build(db, sid, EP[i], &p);
        if ((strstr(p.body, text) != NULL) != present) {
            fprintf(stderr, "FAIL endpoint %d: \"%s\" %s\n", (int)EP[i], text,
                    present ? "missing" : "still sent");
            abort();
        }
        llm_payload_release(&p);
    }
}

static const EndpointType O = ENDPOINT_OPENAI, G = ENDPOINT_GEMINI, A = ENDPOINT_ANTHROPIC;

static void apply_ok(sqlite3 *db, const T *t, const char *reply) {
    char *err = NULL;
    TurnReviewResult rc = turn_review_apply(db, t->sid, reply, "haiku", &err);
    if (rc != TURN_REVIEW_APPLIED) {
        fprintf(stderr, "FAIL apply %s: rc=%d %s\n", reply, (int)rc, err ? err : "");
        abort();
    }
    assert(err == NULL);
    /* The answer keeps its identity and stays the leaf. */
    assert(db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", t->sid, -1) == t->fin);
    assert(sqlite3_get_autocommit(db));
}

/* ── Tests ─────────────────────────────────────────────────────────*/

static void test_untouched_turn_shape(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "base", 0);
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c2,=c3|tool:r2,<c2|tool:r1,<c1"
        "|tool:r3,<c3|assistant:two,=c4|tool:r4,<c4|assistant:done");
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details[0].text", "why1");
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_untouched_turn_shape\n");
}

static void test_input(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "input", 1);
    /* Extra calls in the second iteration: unparseable arguments, a
     * credential capture, a job still running, a tool nobody registered. */
    run(db, "UPDATE entries SET content='oops' WHERE id=%lld", (long long)t.c2);
    run(db, "UPDATE entries SET is_error=1 WHERE id=%lld", (long long)t.r3);

    char *in = turn_review_input(db, t.sid);
    assert(in);
    #define IN(path, want) expect_at(db, in, path, want)
    IN("$.event", "turnSettled");
    IN("$.agent", "default");
    expect_id_at(db, in, "$.session_id", t.sid);
    IN("$.turn_id", "2");
    expect_json(db, in, "json_array_length(?1,'$.user_messages')", "1");
    expect_id_at(db, in, "$.user_messages[0].entry_id", t.u);
    IN("$.user_messages[0].text", "go");
    IN("$.limits.note_max_chars", "600");

    expect_json(db, in, "json_array_length(?1,'$.iterations')", "2");
    expect_id_at(db, in, "$.iterations[0].iteration_id", t.it1);
    IN("$.iterations[0].text", "one");
    /* Calls in call order, whatever order their results came back in. */
    expect_json(db, in, "json_array_length(?1,'$.iterations[0].calls')", "3");
    IN("$.iterations[0].calls[0].call_id", "c1");
    IN("$.iterations[0].calls[0].tool", "look");
    IN("$.iterations[0].calls[0].args.path", "/a");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[0].protected')", "false");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[0].is_error')", "false");
    IN("$.iterations[0].calls[0].result.lines", "10");
    expect_id_at(db, in, "$.iterations[0].calls[0].result.entry_id", t.r1);
    expect_id_at(db, in, "$.iterations[0].calls[0].result.tokens",
                 db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE id=?", t.r1, -1));
    IN("$.iterations[0].calls[0].result.text",
       "1| line-01" PAD "\n2| line-02" PAD "\n3| line-03" PAD "\n4| line-04" PAD
       "\n5| line-05" PAD "\n6| line-06" PAD "\n7| line-07" PAD "\n8| line-08" PAD
       "\n9| line-09" PAD "\n10| line-10" PAD);
    /* Arguments that are not JSON come through as the stored string. */
    IN("$.iterations[0].calls[1].call_id", "c2");
    IN("$.iterations[0].calls[1].args", "oops");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[1].args')", "text");
    IN("$.iterations[0].calls[1].protected", "0");
    /* An effect tool is protected; its failed result is flagged. */
    IN("$.iterations[0].calls[2].call_id", "c3");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[2].protected')", "true");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[2].is_error')", "true");
    /* A trailing newline ends the last line; it does not open a fifth. */
    IN("$.iterations[1].text", "two");
    IN("$.iterations[1].calls[0].result.lines", "4");
    IN("$.iterations[1].calls[0].result.text", "1| w-1" PAD "\n2| w-2" PAD "\n3| w-3" PAD "\n4| w-4" PAD);
    /* The answer is reported apart and is no iteration. */
    IN("$.final.text", "done");
    expect_id_at(db, in, "$.final.entry_id", t.fin);

    /* The sizes the caller decides on. */
    int turn = 0, tools = 0;
    assert(turn_review_measure(db, t.sid, &turn, &tools) == 0);
    expect_id_at(db, in, "$.tokens.turn", turn);
    expect_id_at(db, in, "$.tokens.tool_traffic", tools);
    expect_q(db, "1", "SELECT %d=(SELECT SUM(token_estimate) FROM entries"
                      " WHERE session_id=%lld AND turn_id=2)", turn, (long long)t.sid);
    expect_q(db, "1", "SELECT %d=(SELECT SUM(token_estimate) FROM entries"
                      " WHERE session_id=%lld AND turn_id=2"
                      "   AND type IN ('tool_call','tool_result'))", tools, (long long)t.sid);
    assert(tools > 0 && tools < turn);
    free(in);
    #undef IN

    /* Per-call protection: a credential capture, a job still running, and
     * a tool with no tools row are all protected, whatever the tool says. */
    T p = seed_turn(db, "protect", 0);
    run(db, "UPDATE entries SET content='{\"url\":\"u\",\"save_secret\":\"TOKEN\"}',"
            " tool_name='fetch' WHERE id=%lld", (long long)p.c1);
    run(db, "UPDATE tool_calls SET status='background' WHERE entry_id=%lld", (long long)p.c2);
    run(db, "UPDATE entries SET tool_name='gone' WHERE id=%lld", (long long)p.c4);
    in = turn_review_input(db, p.sid);
    assert(in);
    expect_json(db, in,
        "(SELECT json_group_array(json_extract(c.value,'$.protected'))"
        "  FROM json_each(?1,'$.iterations') i, json_each(i.value,'$.calls') c)",
        "[1,1,1,1]");
    free(in);
    run(db, "UPDATE entries SET content='{\"url\":\"u\"}' WHERE id=%lld", (long long)p.c1);
    in = turn_review_input(db, p.sid);
    assert(in);
    expect_at(db, in, "$.iterations[0].calls[0].protected", "0");
    free(in);

    /* No finished turn: the leaf is a tool result, or there is no entry. */
    int64_t s2 = session_create(db, "midturn", "default", -1, 0);
    assert(turn_review_input(db, s2) == NULL);
    assert(turn_review_measure(db, s2, &turn, &tools) == -1);
    add_user(db, s2, "go");
    int64_t it = db_next_iteration_id(db, s2);
    add_typed(db, s2, it, "assistant_message", "a", NULL, NULL);
    add_call(db, s2, it, "x1", "look", "{}", "done");
    add_typed(db, s2, it, "tool_result", "r", "x1", "look");
    assert(turn_review_input(db, s2) == NULL);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_input\n");
}

/* A refused or empty reply leaves the database byte for byte as it was. */
static void expect_untouched(sqlite3 *db, int64_t sid, const char *reply,
                             TurnReviewResult want, const char *why) {
    char *before = dump(db);
    char *err = NULL;
    TurnReviewResult rc = turn_review_apply(db, sid, reply, "haiku", &err);
    char *after = dump(db);
    if (rc != want || strcmp(before, after) != 0 ||
        (why ? !err || !strstr(err, why) : err != NULL)) {
        fprintf(stderr, "FAIL reply %s\n  rc=%d (want %d) err=%s (want \"%s\") db %s\n",
                reply, (int)rc, (int)want, err ? err : "(none)", why ? why : "(none)",
                strcmp(before, after) ? "CHANGED" : "unchanged");
        abort();
    }
    assert(sqlite3_get_autocommit(db));
    free(before); free(after); free(err);
}

static void test_rejections(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "reject", 1);
    /* Two more calls in the second iteration: a job still running (answered
     * with its "started" line) and a call that never got a result. */
    run(db, "UPDATE sessions SET leaf_id=%lld WHERE id=%lld", (long long)t.r4,
        (long long)t.sid);
    add_call(db, t.sid, t.it2, "c5", "look", "{}", "background");
    add_typed(db, t.sid, t.it2, "tool_result", "started", "c5", "look");
    int64_t c6 = add_call(db, t.sid, t.it2, "c6", "look", "{}", "done");
    run(db, "UPDATE entries SET parent_id=%lld WHERE id=%lld", (long long)c6,
        (long long)t.rs3);
    run(db, "UPDATE sessions SET leaf_id=%lld WHERE id=%lld", (long long)t.fin,
        (long long)t.sid);

    const TurnReviewResult NO = TURN_REVIEW_REJECTED;
    #define REJECT(reply, why) expect_untouched(db, t.sid, reply, NO, why)
    /* Shape. */
    REJECT("nope", "not a JSON object");
    REJECT("[]", "not a JSON object");
    REJECT("\"drop\"", "not a JSON object");
    REJECT("{\"drop\":[\"c1\"],\"summary\":\"x\"}", "unknown key \"summary\"");
    REJECT("{\"drop\":[],\"drop\":[\"c1\"]}", "appears twice");
    REJECT("{\"drop\":\"c1\"}", "\"drop\" must be an array");
    REJECT("{\"drop\":[\"c1\",4]}", "drop[1] must be a call id string");
    REJECT("{\"trim\":{}}", "\"trim\" must be an array");
    REJECT("{\"trim\":[\"c1\"]}", "trim[0] must be");
    REJECT("{\"trim\":[{\"call_id\":\"c1\"}]}", "trim[0] must be");
    REJECT("{\"trim\":[{\"call_id\":7,\"keep\":[[1,2]]}]}", "trim[0] must be");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":7}]}", "trim[0] must be");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,2]],\"why\":\"x\"}]}", "trim[0] must be");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[3]}]}", "keep[0] must be a pair");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[\"1-2\"]}]}", "keep[0] must be a pair");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,2],[4]]}]}", "keep[1] must be a pair");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,2,3]]}]}", "keep[0] must be a pair");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,2.5]]}]}", "keep[0] must be a pair");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[\"1\",2]]}]}", "keep[0] must be a pair");
    REJECT("{\"note\":5}", "\"note\" must be a string");
    REJECT("{\"note\":null}", "\"note\" must be a string");
    REJECT("{\"note\":[\"a\"]}", "\"note\" must be a string");

    /* Call ids. */
    REJECT("{\"drop\":[\"zz\"]}", "\"zz\" is not a tool call of this turn");
    REJECT("{\"drop\":[\"c1\",\"c1\"]}", "listed twice");
    REJECT("{\"drop\":[\"c1\"],\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,1]]}]}",
           "both drop and trim");
    REJECT("{\"trim\":[{\"call_id\":\"zz\",\"keep\":[[1,1]]}]}", "not a tool call");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,1]]},"
           "{\"call_id\":\"c1\",\"keep\":[[2,2]]}]}", "listed twice");
    /* A valid drop beside a bad one is refused with it. */
    REJECT("{\"drop\":[\"c2\",\"c3\"]}", "\"c3\" is protected");
    REJECT("{\"drop\":[\"c5\"]}", "\"c5\" is not finished (background)");
    REJECT("{\"trim\":[{\"call_id\":\"c5\",\"keep\":[[1,1]]}]}", "not finished");
    REJECT("{\"trim\":[{\"call_id\":\"c6\",\"keep\":[[1,1]]}]}", "no result to trim");

    /* Ranges (c1's result has 10 lines). */
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[]}]}", "that is a drop");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[0,2]]}]}", "outside 1..10");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[3,2]]}]}", "outside 1..10");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[2,11]]}]}", "outside 1..10");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[-3,-1]]}]}", "outside 1..10");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,9223372036854775807]]}]}", "outside");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,3],[3,5]]}]}", "overlaps or precedes");
    REJECT("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[5,6],[1,2]]}]}", "overlaps or precedes");
    REJECT("{\"drop\":[\"c2\"],\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,2],[2,2]]}]}",
           "overlaps or precedes");

    /* The note: the cap counts characters, and a credential refuses it. */
    char big[1400] = "{\"note\":\"";
    for (int i = 0; i < TURN_REVIEW_NOTE_MAX_CHARS + 1; i++) strcat(big, "x");
    strcat(big, "\"}");
    REJECT(big, "note is 601 characters; the limit is 600");
    REJECT("{\"note\":\"token was ghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzQ\"}",
           "note contains a secret");
    REJECT("{\"drop\":[\"c2\"],\"note\":\"key AKIAIOSFODNN7EXAMPLE\"}", "contains a secret");

    /* Nothing asked for: no error, and still nothing written. */
    #define NOTHING(reply) expect_untouched(db, t.sid, reply, TURN_REVIEW_NOTHING, NULL)
    NOTHING("{}");
    NOTHING(" { } ");
    NOTHING("{\"drop\":[],\"trim\":[],\"note\":\"\"}");
    /* Keeping every line is no trim. */
    NOTHING("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,10]]}]}");
    NOTHING("{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,4],[5,10]]}]}");
    #undef NOTHING

    /* A caller's open transaction is not ours to commit. */
    run(db, "BEGIN IMMEDIATE");
    char *err = NULL;
    assert(turn_review_apply(db, t.sid, "{\"drop\":[\"c2\"]}", "haiku", &err)
           == TURN_REVIEW_ERROR);
    assert(err && strstr(err, "inside a transaction"));
    free(err);
    run(db, "ROLLBACK");

    /* The turn itself: not finished. */
    run(db, "UPDATE tool_calls SET status='running' WHERE call_id='c4'");
    REJECT("{\"drop\":[\"c2\"]}", "\"c4\" is still running");
    run(db, "UPDATE tool_calls SET status='done' WHERE call_id='c4'");
    run(db, "UPDATE sessions SET leaf_id=%lld WHERE id=%lld", (long long)t.r4,
        (long long)t.sid);
    REJECT("{\"drop\":[\"c2\"]}", "not an assistant message");
    REJECT("{\"note\":\"n\"}", "not an assistant message");
    run(db, "UPDATE sessions SET leaf_id=%lld WHERE id=%lld", (long long)t.fin,
        (long long)t.sid);
    /* The next turn has opened: its calls are not this reply's to name. */
    int64_t s2 = session_create(db, "moved-on", "default", -1, 0);
    add_user(db, s2, "one");
    add_typed(db, s2, db_next_iteration_id(db, s2), "assistant_message", "a", NULL, NULL);
    add_user(db, s2, "two");
    expect_untouched(db, s2, "{\"note\":\"n\"}", NO, "not an assistant message");
    expect_untouched(db, 99999, "{\"note\":\"n\"}", NO, "not an assistant message");

    /* Once reviewed, a turn is not reviewed again. */
    apply_ok(db, &t, "{\"drop\":[\"c2\"]}");
    REJECT("{\"drop\":[\"c1\"]}", "already reviewed");
    REJECT("{\"note\":\"again\"}", "already reviewed");
    #undef REJECT

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_rejections\n");
}

static void test_drop_one_of_batch(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "drop1", 0);
    int before = 0;
    assert(turn_review_measure(db, t.sid, &before, NULL) == 0);
    char *c2_row = q(db, "SELECT json_array(parent_id, original_parent_id, content)"
                         " FROM entries WHERE id=%lld", (long long)t.c2);

    apply_ok(db, &t, "{\"drop\":[\"c2\"]}");
    int64_t rv = review_entry(db, &t);

    /* The call and its result are off the branch, and so is the reasoning
     * entry of the iteration that kept its other calls; the review entry
     * sits before the answer's iteration. */
    expect_branch(db, t.sid, "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld",
        (long long)t.u0, (long long)t.a0, (long long)t.u, (long long)t.a1, (long long)t.c1,
        (long long)t.c3, (long long)t.r1, (long long)t.r3, (long long)t.rs2, (long long)t.a2,
        (long long)t.c4, (long long)t.r4, (long long)rv, (long long)t.rs3, (long long)t.fin);

    /* Re-pointed rows remember the parent they were written under; rows that
     * left are exactly as they were. */
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.rs1, (long long)t.u, (long long)t.a1);
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.c2, (long long)t.c1, (long long)t.c3);
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.r2, (long long)t.c3, (long long)t.r1);
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.r4, (long long)rv, (long long)t.rs3);
    expect_q(db, "4", "SELECT COUNT(*) FROM entries WHERE session_id=%lld"
                      " AND original_parent_id IS NOT NULL", (long long)t.sid);
    expect_q(db, c2_row, "SELECT json_array(parent_id, original_parent_id, content)"
                         " FROM entries WHERE id=%lld", (long long)t.c2);
    free(c2_row);
    expect_q(db, "3", "SELECT COUNT(*) FROM entries WHERE id IN (%lld,%lld,%lld)"
                      " AND original_parent_id IS NULL", (long long)t.rs1, (long long)t.c2,
             (long long)t.r2);

    /* The review entry: a role-0 record in the reviewed turn, no text. */
    expect_q(db, "turn_review|0|2|0||1",
             "SELECT concat_ws('|', type, role, turn_id, iteration_id, COALESCE(content,''),"
             " content IS NULL) FROM entries WHERE id=%lld", (long long)rv);
    char want[128];
    snprintf(want, sizeof(want), "[%lld,%lld,%lld]", (long long)t.rs1, (long long)t.c2,
             (long long)t.r2);
    expect_q(db, want, "SELECT json_extract(data,'$.dropped') FROM entries WHERE id=%lld",
             (long long)rv);
    expect_q(db, "[]|haiku", "SELECT json_extract(data,'$.trimmed') || '|' ||"
             " json_extract(data,'$.reviewer_model') FROM entries WHERE id=%lld", (long long)rv);
    int after = 0;
    assert(turn_review_measure(db, t.sid, &after, NULL) == 0);
    assert(after < before);
    expect_q(db, "1", "SELECT json_extract(data,'$.tokens_before')=%d AND"
                      " json_extract(data,'$.tokens_after')=%d FROM entries WHERE id=%lld",
             before, after, (long long)rv);
    /* The workflow row still says what happened. */
    expect_q(db, "done", "SELECT status FROM tool_calls WHERE call_id='c2'");

    /* Legal on every format, and the dropped pair is gone from all of them. */
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c3|tool:r1,<c1|tool:r3,<c3"
        "|assistant:two,=c4|tool:r4,<c4|assistant:done");
    expect_shape(db, t.sid, G,
        "user:hi|model:hello|user:go|model:one,=look,=act|user:<look|user:<act"
        "|model:two,=look|user:<look|model:done");
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c3|user:<c1,<c3"
        "|assistant:two,=c4|user:<c4|assistant:done");
    expect_text(db, t.sid, "look2", 0);
    expect_text(db, t.sid, "r2", 0);
    expect_text(db, t.sid, "Turn review", 0);
    /* Reasoning is not replayed beside a subset of its calls; the untouched
     * iteration and the answer keep theirs. */
    expect_body(db, t.sid, O, "$.messages[3].content", "one");
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details", NULL);
    expect_body(db, t.sid, O, "$.messages[6].reasoning_details[0].text", "why2");
    expect_body(db, t.sid, O, "$.messages[8].reasoning_details[0].text", "why3");
    expect_text(db, t.sid, "why1", 0);

    /* The session goes on: the next message opens a new turn under the same
     * leaf, and the planner still sees one well-formed branch. */
    int64_t u3 = add_user(db, t.sid, "next");
    expect_q(db, "1", "SELECT turn_id=3 AND parent_id=%lld FROM entries WHERE id=%lld",
             (long long)t.fin, (long long)u3);
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c3|tool:r1,<c1|tool:r3,<c3"
        "|assistant:two,=c4|tool:r4,<c4|assistant:done|user:next");
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c3|user:<c1,<c3"
        "|assistant:two,=c4|user:<c4|assistant:done*|user:next");

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_drop_one_of_batch\n");
}

static void test_drop_whole_iteration(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "dropit", 0);
    apply_ok(db, &t, "{\"drop\":[\"c4\"]}");
    int64_t rv = review_entry(db, &t);

    /* Its only call gone, the iteration leaves whole: text and reasoning too. */
    expect_branch(db, t.sid, "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld",
        (long long)t.u0, (long long)t.a0, (long long)t.u, (long long)t.rs1, (long long)t.a1,
        (long long)t.c1, (long long)t.c2, (long long)t.c3, (long long)t.r2, (long long)t.r1,
        (long long)t.r3, (long long)rv, (long long)t.rs3, (long long)t.fin);
    char want[128];
    snprintf(want, sizeof(want), "[%lld,%lld,%lld,%lld]", (long long)t.rs2, (long long)t.a2,
             (long long)t.c4, (long long)t.r4);
    expect_q(db, want, "SELECT json_extract(data,'$.dropped') FROM entries WHERE id=%lld",
             (long long)rv);

    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c2,=c3|tool:r2,<c2|tool:r1,<c1"
        "|tool:r3,<c3|assistant:done");
    expect_shape(db, t.sid, G,
        "user:hi|model:hello|user:go|model:one,=look,=look2,=act|user:<look2|user:<look"
        "|user:<act|model:done");
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c2,=c3|user:<c2,<c1,<c3"
        "|assistant:done");
    expect_text(db, t.sid, "two", 0);
    expect_text(db, t.sid, "r4", 0);
    expect_text(db, t.sid, "why2", 0);
    /* The batch nobody touched still replays its reasoning. */
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details[0].text", "why1");

    /* Both droppable calls of a batch gone, the protected one staying: the
     * iteration is partly kept. And every droppable call of a turn gone at
     * once, across iterations. */
    T b = seed_turn(db, "dropboth", 0);
    apply_ok(db, &b, "{\"drop\":[\"c4\",\"c2\",\"c1\"]}");
    expect_shape(db, b.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c3|tool:r3,<c3|assistant:done");
    expect_shape(db, b.sid, G,
        "user:hi|model:hello|user:go|model:one,=act|user:<act|model:done");
    expect_shape(db, b.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c3|user:<c3|assistant:done");
    expect_body(db, b.sid, O, "$.messages[3].reasoning_details", NULL);
    expect_q(db, "9", "SELECT json_array_length(data,'$.dropped') FROM entries"
                      " WHERE type='turn_review' AND session_id=%lld", (long long)b.sid);
    free(branch(db, b.sid));

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_drop_whole_iteration\n");
}

static void test_trim(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "trim", 1);
    /* A result from the network, and a failed one: both facts must follow
     * the excerpt. */
    run(db, "UPDATE entries SET network_hosts='[\"example.com\"]', fence_id='f00d'"
            " WHERE id=%lld", (long long)t.r4);
    run(db, "UPDATE entries SET is_error=1 WHERE id=%lld", (long long)t.r3);
    char *r1_before = q(db, "SELECT json_array(parent_id, original_parent_id, content,"
                            " token_estimate) FROM entries WHERE id=%lld", (long long)t.r1);

    /* Leading + middle + trailing omissions; trailing only (on a protected
     * call — trimming is always allowed); leading only, with two adjacent
     * ranges that leave no gap between them. */
    apply_ok(db, &t,
        "{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[3,4],[7,7]]},"
        "           {\"call_id\":\"c3\",\"keep\":[[1,2]]},"
        "           {\"call_id\":\"c4\",\"keep\":[[3,3],[4,4]]}],"
        " \"note\":\"kept the grep hits; rest is in the entries named\"}");
    int64_t rv = review_entry(db, &t);
    int64_t n1 = db_scalar_i64(db, "SELECT json_extract(data,'$.trimmed[0].replacement')"
                                   " FROM entries WHERE id=?", rv, -1);
    int64_t n3 = db_scalar_i64(db, "SELECT json_extract(data,'$.trimmed[1].replacement')"
                                   " FROM entries WHERE id=?", rv, -1);
    int64_t n4 = db_scalar_i64(db, "SELECT json_extract(data,'$.trimmed[2].replacement')"
                                   " FROM entries WHERE id=?", rv, -1);
    assert(n1 > t.fin && n3 > n1 && n4 > n3);

    /* Exact text: kept lines in order, one marker line per omitted run. */
    char want[1024];
    snprintf(want, sizeof(want),
             "[… 2 lines omitted; full output is entry %lld …]\nline-03" PAD "\nline-04" PAD "\n"
             "[… 2 lines omitted; full output is entry %lld …]\nline-07" PAD "\n"
             "[… 3 lines omitted; full output is entry %lld …]",
             (long long)t.r1, (long long)t.r1, (long long)t.r1);
    expect_q(db, want, "SELECT content FROM entries WHERE id=%lld", (long long)n1);
    snprintf(want, sizeof(want), "a-1" PAD "\na-2" PAD "\n[… 3 lines omitted; full output is entry %lld …]",
             (long long)t.r3);
    expect_q(db, want, "SELECT content FROM entries WHERE id=%lld", (long long)n3);
    snprintf(want, sizeof(want), "[… 2 lines omitted; full output is entry %lld …]\nw-3" PAD "\nw-4" PAD,
             (long long)t.r4);
    expect_q(db, want, "SELECT content FROM entries WHERE id=%lld", (long long)n4);
    /* The marker is the header's constant, verbatim. */
    snprintf(want, sizeof(want), TURN_REVIEW_OMIT_FMT, 3, (long long)t.r3);
    expect_q(db, "1", "SELECT instr(content, '%s') > 0 FROM entries WHERE id=%lld", want,
             (long long)n3);

    /* The replacement is the original in every respect but text and size. */
    expect_q(db, "1",
        "SELECT n.type=o.type AND n.role=3 AND n.turn_id=o.turn_id"
        " AND n.iteration_id=o.iteration_id AND n.tool_call_id=o.tool_call_id"
        " AND n.tool_name=o.tool_name AND n.is_error=o.is_error"
        " AND n.network_hosts IS o.network_hosts AND n.fence_id IS o.fence_id"
        " AND n.content_bytes=length(CAST(n.content AS BLOB))"
        " AND n.token_estimate=n.content_bytes/3+4 AND n.token_estimate<o.token_estimate"
        " FROM entries n, entries o WHERE n.id=%lld AND o.id=%lld", (long long)n1,
        (long long)t.r1);
    expect_q(db, "1|c3", "SELECT is_error || '|' || tool_call_id FROM entries WHERE id=%lld",
             (long long)n3);
    expect_q(db, "[\"example.com\"]|f00d", "SELECT network_hosts || '|' || fence_id"
                                           " FROM entries WHERE id=%lld", (long long)n4);
    /* The original stays in the table, untouched, off the branch. */
    expect_q(db, r1_before, "SELECT json_array(parent_id, original_parent_id, content,"
                            " token_estimate) FROM entries WHERE id=%lld", (long long)t.r1);
    free(r1_before);
    expect_branch(db, t.sid, "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld",
        (long long)t.u0, (long long)t.a0, (long long)t.u, (long long)t.rs1, (long long)t.a1,
        (long long)t.c1, (long long)t.c2, (long long)t.c3, (long long)t.r2, (long long)n1,
        (long long)n3, (long long)t.rs2, (long long)t.a2, (long long)t.c4, (long long)n4,
        (long long)rv, (long long)t.rs3, (long long)t.fin);
    expect_q(db, "1", "SELECT parent_id=%lld AND original_parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)n3, (long long)t.r3, (long long)t.rs2);

    /* The record. */
    snprintf(want, sizeof(want),
             "[{\"original\":%lld,\"replacement\":%lld,\"lines_kept\":3,\"lines_total\":10},"
             "{\"original\":%lld,\"replacement\":%lld,\"lines_kept\":2,\"lines_total\":5},"
             "{\"original\":%lld,\"replacement\":%lld,\"lines_kept\":2,\"lines_total\":4}]",
             (long long)t.r1, (long long)n1, (long long)t.r3, (long long)n3, (long long)t.r4,
             (long long)n4);
    expect_q(db, want, "SELECT json_extract(data,'$.trimmed') FROM entries WHERE id=%lld",
             (long long)rv);
    expect_q(db, "[]", "SELECT json_extract(data,'$.dropped') FROM entries WHERE id=%lld",
             (long long)rv);
    expect_q(db, "kept the grep hits; rest is in the entries named",
             "SELECT content FROM entries WHERE id=%lld", (long long)rv);
    /* The excerpt and the note are searchable like any entry. */
    expect_q(db, "1", "SELECT COUNT(*) FROM entries_fts WHERE entries_fts MATCH 'grep'"
                      " AND rowid=%lld", (long long)rv);

    /* On the wire: every call still answered, by the excerpt; the note rides
     * as user text right before the answer. */
    #define NOTE "[Turn review] kept the grep hits; rest is in the entries named"
    expect_body(db, t.sid, O, "$.messages[9].content", NOTE);
    expect_body(db, t.sid, O, "$.messages[9].role", "user");
    expect_body(db, t.sid, O, "$.messages[10].content", "done");
    expect_body(db, t.sid, O, "$.messages[5].tool_call_id", "c1");
    char *n1_text = q(db, "SELECT content FROM entries WHERE id=%lld", (long long)n1);
    expect_body(db, t.sid, O, "$.messages[5].content", n1_text);
    expect_body(db, t.sid, G, "$.contents[5].parts[0].functionResponse.response.content",
                n1_text);
    expect_body(db, t.sid, A, "$.messages[4].content[1].content", n1_text);
    expect_body(db, t.sid, A, "$.messages[4].content[1].tool_use_id", "c1");
    free(n1_text);
    expect_body(db, t.sid, G, "$.contents[9].parts[0].text", NOTE);
    expect_body(db, t.sid, G, "$.contents[9].role", "user");
    expect_body(db, t.sid, A, "$.messages[7].content[0].text", NOTE);
    expect_body(db, t.sid, A, "$.messages[7].role", "user");
    expect_body(db, t.sid, A, "$.messages[8].content[0].text", "done");
    #undef NOTE
    /* The network result keeps its untrusted-content fence around the excerpt. */
    snprintf(want, sizeof(want),
             "<UNTRUSTED_EXTERNAL_CONTENT id=\"f00d\">\nContacted hosts: [\"example.com\"]\n---\n"
             "[… 2 lines omitted; full output is entry %lld …]\nw-3" PAD "\nw-4" PAD "\n"
             "</UNTRUSTED_EXTERNAL_CONTENT id=\"f00d\">", (long long)t.r4);
    expect_body(db, t.sid, O, "$.messages[8].content", want);
    expect_text(db, t.sid, "line-04", 1);
    expect_text(db, t.sid, "line-05", 0);
    expect_text(db, t.sid, "line-10", 0);
    expect_text(db, t.sid, "a-3", 0);
    expect_text(db, t.sid, "w-1", 0);
    /* No call was dropped, so every iteration still replays its reasoning. */
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details[0].text", "why1");
    expect_body(db, t.sid, O, "$.messages[7].reasoning_details[0].text", "why2");

    /* Drop and trim in one reply; an empty kept line is still a line. */
    T m = seed_turn(db, "mixed", 1);
    run(db, "UPDATE entries SET content='x" PAD PAD "' || char(10) || char(10) || 'z'"
            " WHERE id=%lld", (long long)m.r3);
    apply_ok(db, &m, "{\"drop\":[\"c2\"],\"trim\":[{\"call_id\":\"c3\",\"keep\":[[2,3]]}]}");
    snprintf(want, sizeof(want), "[… 1 lines omitted; full output is entry %lld …]\n\nz",
             (long long)m.r3);
    expect_q(db, want, "SELECT content FROM entries WHERE session_id=%lld"
                       " AND type='tool_result' AND tool_call_id='c3' AND id>%lld",
             (long long)m.sid, (long long)m.fin);
    expect_shape(db, m.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c3|user:<c1,<c3"
        "|assistant:two,=c4|user:<c4|assistant:done");
    free(branch(db, m.sid));

    /* An excerpt that would not be smaller than the result is not applied:
     * a marker line costs more than the two short lines it stands for. */
    T s = seed_turn(db, "short", 0);
    run(db, "UPDATE entries SET content='a' || char(10) || 'b' || char(10) || 'c'"
            " WHERE id=%lld", (long long)s.r1);
    expect_untouched(db, s.sid, "{\"trim\":[{\"call_id\":\"c1\",\"keep\":[[1,1]]}]}",
                     TURN_REVIEW_NOTHING, NULL);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_trim\n");
}

static void test_note_only(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "note", 0);
    int before = 0, after = 0;
    assert(turn_review_measure(db, t.sid, &before, NULL) == 0);
    /* A zero-width space is stripped on the way in, like any text entering
     * context; the cap is in characters, not bytes. */
    char reply[2048] = "{\"note\":\"all of it m\xE2\x80\x8B" "attered — ";
    for (int i = 0; i < TURN_REVIEW_NOTE_MAX_CHARS - 21; i++) strcat(reply, "\xC3\xA9");
    strcat(reply, "\"}");
    apply_ok(db, &t, reply);
    int64_t rv = review_entry(db, &t);

    expect_branch(db, t.sid, "%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld,%lld",
        (long long)t.u0, (long long)t.a0, (long long)t.u, (long long)t.rs1, (long long)t.a1,
        (long long)t.c1, (long long)t.c2, (long long)t.c3, (long long)t.r2, (long long)t.r1,
        (long long)t.r3, (long long)t.rs2, (long long)t.a2, (long long)t.c4, (long long)t.r4,
        (long long)rv, (long long)t.rs3, (long long)t.fin);
    expect_q(db, "1", "SELECT COUNT(*) FROM entries WHERE session_id=%lld"
                      " AND original_parent_id IS NOT NULL", (long long)t.sid);
    expect_q(db, "all of it mattered — ", "SELECT substr(content, 1, 21) FROM entries"
                                           " WHERE id=%lld", (long long)rv);
    expect_q(db, "600", "SELECT length(content) FROM entries WHERE id=%lld", (long long)rv);
    expect_q(db, "0", "SELECT instr(content, char(8203)) FROM entries WHERE id=%lld",
             (long long)rv);
    expect_q(db, "[]|[]", "SELECT json_extract(data,'$.dropped') || '|' ||"
             " json_extract(data,'$.trimmed') FROM entries WHERE id=%lld", (long long)rv);
    /* The note is the one thing the review added to the turn. */
    assert(turn_review_measure(db, t.sid, &after, NULL) == 0);
    expect_q(db, "1", "SELECT %d - %d = token_estimate AND"
                      " json_extract(data,'$.tokens_after')=%d FROM entries WHERE id=%lld",
             after, before, after, (long long)rv);

    expect_body(db, t.sid, O, "$.messages[9].role", "user");
    expect_body(db, t.sid, O, "$.messages[10].content", "done");
    expect_text(db, t.sid, "[Turn review] all of it mattered — ", 1);
    /* Nothing was dropped: every reasoning replay is intact. */
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details[0].text", "why1");
    expect_body(db, t.sid, O, "$.messages[10].reasoning_details[0].text", "why3");
    expect_body(db, t.sid, G, "$.contents[9].role", "user");
    expect_body(db, t.sid, G, "$.contents[10].parts[0].text", "done");
    expect_body(db, t.sid, A, "$.messages[7].role", "user");
    expect_body(db, t.sid, A, "$.messages[8].content[0].text", "done");

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_note_only\n");
}

/* The answer has no reasoning entry (most providers): the review entry then
 * sits directly under the answer. And a reviewer model is optional. */
static void test_plain_answer(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "plain", "default", -1, 0);
    int64_t u = add_user(db, sid, "go");
    int64_t it = db_next_iteration_id(db, sid);
    int64_t a = add_typed(db, sid, it, "assistant_message", NULL, NULL, NULL);
    int64_t c = add_call(db, sid, it, "k1", "look", "{}", "done");
    int64_t r = add_typed(db, sid, it, "tool_result", "one" PAD "\ntwo\nthree" PAD, "k1", "look");
    int64_t fin = add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message",
                            "done", NULL, NULL);
    assert(r == 4);     /* named in the expected marker text below */
    assert(turn_review_apply(db, sid, "{\"trim\":[{\"call_id\":\"k1\",\"keep\":[[2,2]]}]}",
                             NULL, NULL) == TURN_REVIEW_APPLIED);
    int64_t rv = db_scalar_i64(db, "SELECT id FROM entries WHERE type='turn_review'"
                                   " AND session_id=?", sid, -1);
    int64_t nr = db_scalar_i64(db, "SELECT MAX(id) FROM entries WHERE type='tool_result'"
                                   " AND session_id=?", sid, -1);
    char want[160];
    snprintf(want, sizeof(want), "%lld,%lld,%lld,%lld,%lld,%lld", (long long)u, (long long)a,
             (long long)c, (long long)nr, (long long)rv, (long long)fin);
    char *got = branch(db, sid);
    assert(strcmp(got, want) == 0);
    free(got);
    assert(nr != r);
    expect_q(db, "null", "SELECT json_type(data,'$.reviewer_model') FROM entries WHERE id=%lld",
             (long long)rv);
    assert(db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", sid, -1) == fin);
    expect_shape(db, sid, O, "user:go|assistant:=k1|tool:"
        "[… 1 lines omitted; full output is entry 4 …]\ntwo\n"
        "[… 1 lines omitted; full output is entry 4 …],<k1|assistant:done");

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_plain_answer\n");
}

/* Around the edges of a turn: an earlier compaction already re-pointed the
 * turn's first entry (its original_parent_id is compaction's to keep), and
 * the result was written under an iteration id of its own, as a sub-agent's
 * answer or an approval notice is — it still leaves with its call. */
static void test_turn_edges(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "edges", "default", -1, 0);
    int64_t u0 = add_user(db, sid, "hi");
    int64_t a0 = add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message",
                           "hello", NULL, NULL);
    int64_t u = add_user(db, sid, "go");
    int64_t it = db_next_iteration_id(db, sid);
    add_typed(db, sid, it, "assistant_message", "looking", NULL, NULL);
    add_call(db, sid, it, "k1", "look", "{}", "done");
    add_typed(db, sid, db_next_iteration_id(db, sid), "tool_result", "nothing there",
              "k1", "look");
    int64_t fin = add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message",
                            "done", NULL, NULL);
    char *in = turn_review_input(db, sid);
    assert(in);
    expect_at(db, in, "$.iterations[0].calls[0].result.text", "1| nothing there");
    free(in);
    int64_t cmp = entry_compact(db, sid, u0, u, "sum");
    assert(cmp > 0);
    expect_shape(db, sid, O, "user:hi|user:[Summary of earlier conversation]\nsum|user:go"
                             "|assistant:looking,=k1|tool:nothing there,<k1|assistant:done");

    assert(turn_review_apply(db, sid, "{\"drop\":[\"k1\"]}", "haiku", NULL)
           == TURN_REVIEW_APPLIED);
    int64_t rv = db_scalar_i64(db, "SELECT id FROM entries WHERE type='turn_review'"
                                   " AND session_id=?", sid, -1);
    char want[96];
    snprintf(want, sizeof(want), "%lld,%lld,%lld,%lld,%lld", (long long)u0, (long long)cmp,
             (long long)u, (long long)rv, (long long)fin);
    char *got = branch(db, sid);
    assert(strcmp(got, want) == 0);
    free(got);
    expect_q(db, "1", "SELECT parent_id=%lld AND original_parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)cmp, (long long)a0, (long long)u);
    assert(db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", sid, -1) == fin);
    expect_shape(db, sid, O, "user:hi|user:[Summary of earlier conversation]\nsum|user:go"
                             "|assistant:done");
    expect_shape(db, sid, G, "user:hi|user:[Summary of earlier conversation]\nsum|user:go"
                             "|model:done");
    expect_shape(db, sid, A, "user:hi|user:[Summary of earlier conversation]\nsum*|user:go"
                             "|assistant:done");

    /* Compaction after a review: the reviewed turn is summarized away like
     * any other, review entry included. */
    int64_t u2 = add_user(db, sid, "more");
    assert(entry_compact(db, sid, u0, u2, "sum2") > 0);
    expect_shape(db, sid, O, "user:hi|user:[Summary of earlier conversation]\nsum2|user:more");

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_turn_edges\n");
}

int main(void) {
    TEST_INIT();
    printf("test_turn_review:\n");
    test_untouched_turn_shape();
    test_input();
    test_rejections();
    test_drop_one_of_batch();
    test_drop_whole_iteration();
    test_trim();
    test_note_only();
    test_plain_answer();
    test_turn_edges();
    printf("All turn review tests passed.\n");
    return 0;
}
