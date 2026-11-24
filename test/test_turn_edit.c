/* turn_edit.c: the finished turn as a hook's input, and the edits a
 * Stop or ContextThreshold script sends back — checked, then applied all or nothing — on a
 * real temp DB, with the requests built afterwards on all three wire formats. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "buf.h"
#include "config.h"
#include "context.h"
#include "llm_payload.h"
#include "turn_edit.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DB_PATH = "/tmp/test_cclaw_turn_edit.db";
#define LABEL "hook:rev"
#define NOTE_TAG "[note from extension rev] "

/* ── Fixtures ──────────────────────────────────────────────────────*/

static sqlite3 *open_seeded(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    test_seed_agent(db, "default");
    /* Tool facts as tools_sync_to_db / a promoted manifest leave them: two
     * look-up tools, one whose call is its effect, one retrieving tool that
     * can also store a credential, and check_session (never droppable). */
    assert(sqlite3_exec(db,
        "INSERT INTO tools(name, droppable) VALUES"
        " ('look',1),('look2',1),('act',0),('fetch',1),('check_session',0);",
        NULL, NULL, NULL) == SQLITE_OK);
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

/* A one-value query over a JSON document bound as ?1. */
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

/* Length of the JSON text at `path` in `json`, as written out. */
static int64_t json_len_at(sqlite3 *db, const char *json, const char *path) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT octet_length(json_extract(?1, ?2))", -1, &st,
                              NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, json, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int64_t n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

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

/* What a Stop hook is sent. */
static const TurnEditInputFor STOP = { .event = "Stop", .need_answer = 1 };

/* The input as a hook that declares nothing gets it: the finished turn. */
static char *input_turn(sqlite3 *db, int64_t sid) {
    TurnEditInputSpec spec = TURN_EDIT_INPUT_DEFAULTS;
    return turn_edit_input(db, sid, &spec, &STOP, NULL);
}

/* ...and as one that asked for everything, with room for it. */
static char *input_all(sqlite3 *db, int64_t sid) {
    TurnEditInputSpec spec = { TURN_EDIT_HISTORY_ALL, 1, TURN_EDIT_INPUT_MAX_BYTES };
    return turn_edit_input(db, sid, &spec, &STOP, NULL);
}

static char *input_as(sqlite3 *db, int64_t sid, int history, int results, int max_bytes,
                      int *too_big) {
    TurnEditInputSpec spec = { history, results, max_bytes };
    return turn_edit_input(db, sid, &spec, &STOP, too_big);
}

static int64_t add_user(sqlite3 *db, int64_t sid, const char *text) {
    Message m = {.role = ROLE_USER, .content = (char *)text};
    int64_t id = entry_append_with_iteration(db, sid, &m, 0);
    assert(id > 0);
    return id;
}

static int64_t add_system(sqlite3 *db, int64_t sid, const char *text) {
    Message m = {.role = ROLE_SYSTEM, .content = (char *)text};
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

typedef struct {
    int64_t sid, turn;
    int64_t u0, a0;                                  /* an earlier turn */
    int64_t u;
    int64_t it1, rs1, a1, c1, c2, c3, r2, r1, r3;    /* a parallel batch */
    int64_t sys;                                     /* SEED_NOTE: a system entry */
    int64_t it2, rs2, a2, c4, r4;
    int64_t it3, rs3, fin;                           /* the answer */
} T;

#define SEED_NOTE 1

/* A finished turn: a three-call batch answered out of order, a second
 * iteration with one call, the answer. With SEED_NOTE an ordinary system
 * entry sits between the two iterations. */
static T seed_turn(sqlite3 *db, const char *name, int flags) {
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
    t.r1 = add_typed(db, t.sid, t.it1, "tool_result", "r1", "c1", "look");
    t.r3 = add_typed(db, t.sid, t.it1, "tool_result", "r3", "c3", "act");
    if (flags & SEED_NOTE) t.sys = add_system(db, t.sid, "sys mid");

    t.it2 = db_next_iteration_id(db, t.sid);
    t.rs2 = add_reasoning(db, t.sid, t.it2, "why2");
    t.a2 = add_typed(db, t.sid, t.it2, "assistant_message", "two", NULL, NULL);
    t.c4 = add_call(db, t.sid, t.it2, "c4", "look", "{}", "done");
    t.r4 = add_typed(db, t.sid, t.it2, "tool_result", "r4", "c4", "look");

    t.it3 = db_next_iteration_id(db, t.sid);
    t.rs3 = add_reasoning(db, t.sid, t.it3, "why3");
    t.fin = add_typed(db, t.sid, t.it3, "assistant_message", "done", NULL, NULL);

    t.turn = db_scalar_i64(db, "SELECT turn_id FROM entries WHERE id=?", t.fin, -1);
    assert(t.turn == 2);
    assert(db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", t.sid, -1) == t.fin);
    if (t.sys)
        assert(db_scalar_i64(db, "SELECT turn_id FROM entries WHERE id=?", t.sys, -1) == 2);
    return t;
}

/* Everything an apply could touch, as one string. */
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

/* "id,id,..." from a va_list of ids ended by 0. */
static void ids_str(char *out, size_t cap, va_list ap) {
    out[0] = '\0';
    for (int64_t id; (id = va_arg(ap, int64_t)) != 0;) {
        size_t n = strlen(out);
        snprintf(out + n, cap - n, "%s%lld", n ? "," : "", (long long)id);
    }
}

/* Expected branch from a list of ids ended by 0. */
static void expect_branch_ids(sqlite3 *db, int64_t sid, ...) {
    char want[512];
    va_list ap;
    va_start(ap, sid);
    ids_str(want, sizeof(want), ap);
    va_end(ap);
    char *got = branch(db, sid);
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL branch:\n  got  %s\n  want %s\n", got, want);
        abort();
    }
    free(got);
}
#define BRANCH(db, sid, ...) expect_branch_ids(db, sid, __VA_ARGS__, (int64_t)0)

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

static void apply_as(sqlite3 *db, const T *t, const char *label, const char *reply) {
    char *err = NULL;
    TurnEditResult rc = turn_edit_apply(db, t->sid, reply, label, &err);
    if (rc != TURN_EDIT_APPLIED) {
        fprintf(stderr, "FAIL apply %s: rc=%d %s\n", reply, (int)rc, err ? err : "");
        abort();
    }
    assert(err == NULL);
    /* The answer keeps its identity and stays the leaf. */
    assert(db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", t->sid, -1) == t->fin);
    assert(sqlite3_get_autocommit(db));
}

static void apply_ok(sqlite3 *db, const T *t, const char *reply) {
    apply_as(db, t, LABEL, reply);
}

/* The newest element of $.turn_edits on the answer, at `path` within it. */
static void expect_rec(sqlite3 *db, const T *t, const char *path, const char *want) {
    expect_q(db, want, "SELECT json_extract(data,'$.turn_edits[#-1]%s') FROM entries"
                       " WHERE id=%lld", path, (long long)t->fin);
}

static void expect_rec_ids(sqlite3 *db, const T *t, const char *path, ...) {
    char ids[256], want[260];
    va_list ap;
    va_start(ap, path);
    ids_str(ids, sizeof(ids), ap);
    va_end(ap);
    snprintf(want, sizeof(want), "[%s]", ids);
    expect_rec(db, t, path, want);
}
#define REC_IDS(db, t, path, ...) expect_rec_ids(db, t, path, __VA_ARGS__, (int64_t)0)

/* The size of turn 2, or (turn 0) of the whole branch from the answer back —
 * what an edit's record measures. */
static int64_t tokens_of(sqlite3 *db, const T *t, int turn) {
    char *v = q(db,
        "WITH RECURSIVE b(id, lvl) AS (SELECT %lld, 0 UNION ALL"
        "  SELECT e.parent_id, b.lvl+1 FROM entries e JOIN b ON e.id=b.id WHERE b.lvl<1000)"
        " SELECT SUM(e.token_estimate) FROM b JOIN entries e ON e.id=b.id"
        "  WHERE %d=0 OR e.turn_id=%d", (long long)t->fin, turn, turn);
    int64_t n = v ? atoll(v) : -1;
    free(v);
    return n;
}
#define turn_tokens(db, t) tokens_of(db, t, 2)
#define branch_tokens(db, t) tokens_of(db, t, 0)

/* The newest record on the session's leaf, at `path` within it. */
static int64_t leaf_rec_id(sqlite3 *db, int64_t sid, const char *path) {
    char *v = q(db, "SELECT json_extract(data,'$.turn_edits[#-1]%s') FROM entries"
                    " WHERE id=(SELECT leaf_id FROM sessions WHERE id=%lld)", path,
                (long long)sid);
    int64_t n = v ? atoll(v) : -1;
    free(v);
    return n;
}

/* ── Tests ─────────────────────────────────────────────────────────*/

static void test_input(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "input", SEED_NOTE);
    run(db, "UPDATE entries SET content='line one' || char(10) || '  line two'"
            " WHERE id=%lld", (long long)t.r1);
    run(db, "UPDATE entries SET content='oops' WHERE id=%lld", (long long)t.c2);
    run(db, "UPDATE entries SET is_error=1 WHERE id=%lld", (long long)t.r3);

    char *in = input_turn(db, t.sid);
    assert(in);
    #define IN(path, want) expect_at(db, in, path, want)
    IN("$.event", "Stop");
    expect_json(db, in, "json_type(?1,'$.context')", NULL);
    /* Nothing declared is the turn alone, and the input says so; the turn
     * before it is not sent and not counted as left out. */
    IN("$.input.history", "none");
    expect_json(db, in, "json_array_length(?1,'$.history')", "0");
    IN("$.history_omitted", "0");
    IN("$.turn_id", "2");
    expect_id_at(db, in, "$.user_messages[0].entry_id", t.u);
    /* The system entry already in the turn, and the iteration it precedes. */
    expect_id_at(db, in, "$.notes[0].entry_id", t.sys);
    expect_id_at(db, in, "$.notes[0].before_iteration", t.it2);
    /* An iteration's entry_id is its first row (the reasoning row); a
     * parallel batch lists calls in call order, whatever order the results
     * came back in; an effect tool is protected and a failed result flagged. */
    expect_id_at(db, in, "$.iterations[0].entry_id", t.rs1);
    expect_id_at(db, in, "$.iterations[1].entry_id", t.rs2);
    char want[256];
    snprintf(want, sizeof(want), "c1:0:0:%lld,c2:0:0:%lld,c3:1:1:%lld,c4:0:0:%lld",
             (long long)t.r1, (long long)t.r2, (long long)t.r3, (long long)t.r4);
    expect_json(db, in,
        "(SELECT group_concat(concat_ws(':', json_extract(c.value,'$.call_id'),"
        "   json_extract(c.value,'$.protected'), json_extract(c.value,'$.is_error'),"
        "   json_extract(c.value,'$.result.entry_id')), ',')"
        "  FROM json_each(?1,'$.iterations') i, json_each(i.value,'$.calls') c)", want);
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[2].protected')", "true");
    /* Results verbatim (no line numbers, whitespace kept); arguments that are
     * not JSON come through as the stored string. */
    IN("$.iterations[0].calls[0].result.text", "line one\n  line two");
    IN("$.iterations[0].calls[0].args.path", "/a");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[1].args')", "text");
    /* The answer is reported apart and is no iteration. */
    expect_json(db, in, "json_array_length(?1,'$.iterations')", "2");
    expect_id_at(db, in, "$.final.entry_id", t.fin);
    IN("$.final.text", "done");
    /* Sizes: the whole turn, and the tool traffic within it. */
    expect_id_at(db, in, "$.tokens.turn", db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=? AND turn_id=2", t.sid, -1));
    expect_id_at(db, in, "$.tokens.tools", db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=? AND turn_id=2"
        " AND type IN ('tool_call','tool_result')", t.sid, -1));
    free(in);
    #undef IN
    /* Per-call protection: a credential capture, a job still running,
     * check_session, and an effect tool are all protected. */
    T p = seed_turn(db, "protect", 0);
    run(db, "UPDATE entries SET content='{\"url\":\"u\",\"save_secret\":\"TOKEN\"}',"
            " tool_name='fetch' WHERE id=%lld", (long long)p.c1);
    run(db, "UPDATE tool_calls SET status='background' WHERE entry_id=%lld", (long long)p.c2);
    run(db, "UPDATE entries SET tool_name='check_session' WHERE id IN (%lld,%lld)",
        (long long)p.c4, (long long)p.r4);
    #define PROTECTED(want) do { in = input_turn(db, p.sid); assert(in); \
        expect_json(db, in, "(SELECT json_group_array(json_extract(c.value,'$.protected'))" \
            " FROM json_each(?1,'$.iterations') i, json_each(i.value,'$.calls') c)", want); \
        free(in); } while (0)
    PROTECTED("[1,1,1,1]");
    /* Without save_secret the same fetch is droppable; a tool with no tools
     * row is protected. */
    run(db, "UPDATE entries SET content='{\"url\":\"u\"}' WHERE id=%lld", (long long)p.c1);
    run(db, "UPDATE entries SET tool_name='unregistered' WHERE id=%lld", (long long)p.c3);
    PROTECTED("[0,1,1,1]");
    #undef PROTECTED

    /* No finished turn: no entry, or the leaf is a tool result. */
    int64_t s2 = session_create(db, "midturn", "default", -1, 0);
    assert(input_turn(db, s2) == NULL);
    add_user(db, s2, "go");
    int64_t it = db_next_iteration_id(db, s2);
    add_typed(db, s2, it, "assistant_message", "a", NULL, NULL);
    add_call(db, s2, it, "x1", "look", "{}", "done");
    add_typed(db, s2, it, "tool_result", "r", "x1", "look");
    assert(input_turn(db, s2) == NULL);

    db_close(db); test_db_clean(DB_PATH);
}

/* A refused or empty reply leaves the database byte for byte as it was. */
static void expect_untouched(sqlite3 *db, int64_t sid, const char *reply,
                             TurnEditResult want, const char *why) {
    char *before = dump(db);
    char *err = NULL;
    TurnEditResult rc = turn_edit_apply(db, sid, reply, LABEL, &err);
    char *after = dump(db);
    if (rc != want || strcmp(before, after) != 0 ||
        (why ? !err || !strstr(err, why) : err != NULL)) {
        fprintf(stderr, "FAIL reply %.200s\n  rc=%d (want %d) err=%s (want \"%s\") db %s\n",
                reply, (int)rc, (int)want, err ? err : "(none)", why ? why : "(none)",
                strcmp(before, after) ? "CHANGED" : "unchanged");
        abort();
    }
    assert(sqlite3_get_autocommit(db));
    free(before); free(after); free(err);
}

#define E(...) "{\"edits\":[" __VA_ARGS__ "]}"
#define DROP(id) "{\"op\":\"drop\",\"call_id\":\"" id "\"}"

static void test_rejections(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "reject", 0);
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

    const TurnEditResult NO = TURN_EDIT_REJECTED;
    #define REJECT(reply, why) expect_untouched(db, t.sid, reply, NO, why)
    /* Every reply here is refused whole, with nothing written. Hook replies
     * come from untrusted scripts: each row is a distinct malformed input. */
    static const struct { const char *reply, *why; } cases[] = {
        /* Shape. */
        { "nope", "not a JSON object" },
        { "[]", "not a JSON object" },
        { "\"drop\"", "not a JSON object" },
        { "", "not a JSON object" },
        { "{\"edits\":[],\"summary\":\"x\"}", "unknown key" },
        { "{\"drop\":[\"c1\"]}", "unknown key" },
        { "{\"edits\":[],\"edits\":[" DROP("c2") "]}", "appears twice" },
        { "{\"edits\":{}}", "must be an array" },
        { "{\"edits\":null}", "must be an array" },
        /* A bad record is refused even with no edits: shape comes first. */
        { "{\"record\":[]}", "\"record\" must be an object" },
        { "{\"edits\":[],\"record\":\"x\"}", "\"record\" must be an object" },
        { "{\"edits\":[" DROP("c2") "],\"record\":5}", "\"record\" must be an object" },
        { E("5"), "edits[0] must be an object" },
        { E(DROP("c2") ",7"), "edits[1] must be an object" },
        { E("{\"op\":\"cut\",\"call_id\":\"c1\"}"), "\"op\" must be" },
        { E("{\"call_id\":\"c1\"}"), "\"op\" must be" },
        { E("{\"op\":1,\"call_id\":\"c1\"}"), "\"op\" must be" },
        { E("{\"op\":\"drop\",\"call_id\":\"c1\",\"call_id\":\"c2\"}"), "twice" },
        { E("{\"op\":\"drop\",\"call_id\":\"c1\",\"text\":\"x\"}"), "does not take" },
        { E("{\"op\":\"drop\",\"call_id\":\"c1\",\"why\":\"x\"}"), "does not take" },
        { E("{\"op\":\"note\",\"text\":\"x\",\"call_id\":\"c1\"}"), "does not take" },
        { E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"x\",\"before\":1}"),
          "does not take" },
        { E("{\"op\":\"drop\"}"), "needs \"call_id\"" },
        { E("{\"op\":\"drop\",\"call_id\":4}"), "needs \"call_id\"" },
        { E("{\"op\":\"replace\",\"call_id\":\"c1\"}"), "needs \"text\"" },
        { E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"\"}"), "needs \"text\"" },
        { E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":5}"), "needs \"text\"" },
        { E("{\"op\":\"note\"}"), "needs \"text\"" },
        { E("{\"op\":\"note\",\"text\":[\"a\"]}"), "needs \"text\"" },
        { E("{\"op\":\"note\",\"text\":\"x\",\"before\":\"5\"}"), "\"before\" must be" },
        { E("{\"op\":\"note\",\"text\":\"x\",\"before\":1.5}"), "\"before\" must be" },
        /* Call ids; a valid drop beside a bad one is refused with it. */
        { E(DROP("zz")), "not a tool call" },
        { E("{\"op\":\"replace\",\"call_id\":\"zz\",\"text\":\"x\"}"), "not a tool call" },
        { E(DROP("c1") "," DROP("c1")), "named by two edits" },
        { E(DROP("c1") ",{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"x\"}"),
          "named by two edits" },
        { E(DROP("c2") "," DROP("c3")), "protected" },
        { E(DROP("c5")), "not finished" },
        { E("{\"op\":\"replace\",\"call_id\":\"c6\",\"text\":\"x\"}"), "no result" },
        /* Text: invisible-only is empty; a credential refuses the reply. */
        { E("{\"op\":\"note\",\"text\":\"\xE2\x80\x8B\"}"), "empty" },
        { E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"\xE2\x80\x8B\xE2\x80\x8C\"}"),
          "empty" },
        { E("{\"op\":\"note\",\"text\":\"token was ghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzQ\"}"),
          "edits[0]: text contains a secret" },
        { E(DROP("c2") ",{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"key AKIAIOSFODNN7EXAMPLE\"}"),
          "edits[1]: text contains a secret" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("    reject[%zu]\n", i);
        expect_untouched(db, t.sid, cases[i].reply, NO, cases[i].why);
    }
    {
        size_t n = TURN_EDIT_RECORD_MAX_BYTES + 64;
        char *big = malloc(n);
        assert(big);
        strcpy(big, "{\"record\":{\"pad\":\"");
        size_t len = strlen(big);
        memset(big + len, 'x', TURN_EDIT_RECORD_MAX_BYTES);
        strcpy(big + len + TURN_EDIT_RECORD_MAX_BYTES, "\"}}");
        REJECT(big, "larger than");
        free(big);
    }

    /* Where a note may stand: only the first row of an iteration of this
     * turn, and not one this reply empties. */
    char r[256];
    const int64_t not_first[] = { t.a1 /* rs1 opens it1 */, t.c1, t.r1, t.u, 99999 };
    for (size_t i = 0; i < sizeof(not_first) / sizeof(not_first[0]); i++) {
        printf("    note before %lld\n", (long long)not_first[i]);
        snprintf(r, sizeof(r), E("{\"op\":\"note\",\"text\":\"n\",\"before\":%lld}"),
                 (long long)not_first[i]);
        REJECT(r, "\"before\"");
    }
    /* (A note before an iteration the reply empties: on a turn of its own
     * below, since c5/c6 keep this turn's second iteration alive.) */

    /* Nothing asked for: no error, and still nothing written. */
    #define NOTHING(reply) expect_untouched(db, t.sid, reply, TURN_EDIT_NOTHING, NULL)
    NOTHING("{}");
    NOTHING(" { } ");
    NOTHING("{\"edits\":[]}");
    NOTHING("{\"record\":{\"why\":\"small turn\"}}");
    NOTHING("{\"edits\":[],\"record\":{}}");
    #undef NOTHING

    /* A caller's open transaction is not ours to commit. */
    run(db, "BEGIN IMMEDIATE");
    char *err = NULL;
    assert(turn_edit_apply(db, t.sid, E(DROP("c2")), LABEL, &err) == TURN_EDIT_ERROR);
    assert(err && strstr(err, "inside a transaction"));
    free(err);
    run(db, "ROLLBACK");

    /* Check 2: a call still running blocks whatever would remove it —
     * itself, its result's stand-in, a rewrite, its whole iteration. */
    run(db, "UPDATE tool_calls SET status='running' WHERE call_id='c4'");
    REJECT(E(DROP("c4")), "running");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"c4\",\"text\":\"x\"}"), "running");
    REJECT(E("{\"op\":\"rewrite\",\"call_id\":\"c4\",\"args\":{}}"), "running");
    snprintf(r, sizeof(r), E("{\"op\":\"summarise\",\"text\":\"s\",\"from_iteration\":%lld,"
             "\"to_iteration\":%lld}"), (long long)t.it1, (long long)t.it2);
    REJECT(r, "running");
    run(db, "UPDATE tool_calls SET status='done' WHERE call_id='c4'");
    expect_untouched(db, 99999, E("{\"op\":\"note\",\"text\":\"n\"}"), NO, "no turn");

    /* One call id on two calls, one call with two results. */
    int64_t s3 = session_create(db, "dups", "default", -1, 0);
    add_user(db, s3, "go");
    int64_t it = db_next_iteration_id(db, s3);
    add_typed(db, s3, it, "assistant_message", "a", NULL, NULL);
    add_call(db, s3, it, "k1", "look", "{}", "done");
    add_call(db, s3, it, "k2", "look", "{}", "done");
    add_call(db, s3, it, "k2", "look", "{}", "done");
    add_typed(db, s3, it, "tool_result", "x", "k1", "look");
    add_typed(db, s3, it, "tool_result", "y", "k1", "look");
    add_typed(db, s3, it, "tool_result", "z", "k2", "look");
    add_typed(db, s3, db_next_iteration_id(db, s3), "assistant_message", "done", NULL, NULL);
    expect_untouched(db, s3, E(DROP("k2")), NO, "more than one call");
    expect_untouched(db, s3, E("{\"op\":\"replace\",\"call_id\":\"k1\",\"text\":\"t\"}"), NO,
                     "more than one result");

    /* A note before an iteration the same reply empties. */
    T d = seed_turn(db, "emptied", 0);
    char r2[256];
    snprintf(r2, sizeof(r2), E(DROP("c4") ",{\"op\":\"note\",\"text\":\"n\",\"before\":%lld}"),
             (long long)d.rs2);
    expect_untouched(db, d.sid, r2, NO, "drops");
    /* An answer whose data is not JSON: our side, refused, nothing written. */
    run(db, "UPDATE entries SET data='not json {' WHERE id=%lld", (long long)d.fin);
    expect_untouched(db, d.sid, E(DROP("c4")), TURN_EDIT_ERROR, "write failed");
    #undef REJECT

    db_close(db); test_db_clean(DB_PATH);
}

static void test_drop_one_of_batch(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "drop1", 0);
    int64_t before = branch_tokens(db, &t);
    char *c2_row = q(db, "SELECT json_array(parent_id, original_parent_id, content)"
                         " FROM entries WHERE id=%lld", (long long)t.c2);

    apply_ok(db, &t, E(DROP("c2")));

    /* The call and its result are off the branch, and so is the reasoning
     * entry of the iteration that kept its other calls. */
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.a1, t.c1, t.c3, t.r1, t.r3, t.rs2, t.a2, t.c4,
           t.r4, t.rs3, t.fin);
    /* Re-pointed rows remember the parent they were written under; rows that
     * left are exactly as they were. */
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.rs1, (long long)t.u, (long long)t.a1);
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.c2, (long long)t.c1, (long long)t.c3);
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.r2, (long long)t.c3, (long long)t.r1);
    expect_q(db, "3", "SELECT COUNT(*) FROM entries WHERE session_id=%lld"
                      " AND original_parent_id IS NOT NULL", (long long)t.sid);
    expect_q(db, c2_row, "SELECT json_array(parent_id, original_parent_id, content)"
                         " FROM entries WHERE id=%lld", (long long)t.c2);
    free(c2_row);
    expect_q(db, "done", "SELECT status FROM tool_calls WHERE call_id='c2'");

    /* The record, on the answer. */
    REC_IDS(db, &t, ".left_branch", t.rs1, t.c2, t.r2);
    expect_rec(db, &t, ".replaced", "[]");
    expect_rec(db, &t, ".notes", "[]");
    expect_rec(db, &t, ".by", LABEL);
    expect_q(db, "1", "SELECT json_extract(data,'$.turn_edits[0].tokens.before')=%lld"
                      " AND json_extract(data,'$.turn_edits[0].tokens.after')=%lld"
                      " AND json_type(data,'$.turn_edits[0].at')='integer'"
                      " AND json_type(data,'$.turn_edits[0].record') IS NULL"
                      " AND json_array_length(data,'$.turn_edits')=1 FROM entries WHERE id=%lld",
             (long long)before, (long long)branch_tokens(db, &t), (long long)t.fin);
    assert(branch_tokens(db, &t) < before);

    /* Legal on every format, and the dropped pair is gone from all of them. */
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c3|tool:r1,<c1|tool:r3,<c3"
        "|assistant:two,=c4|tool:r4,<c4|assistant:done");
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c3|user:<c1,<c3"
        "|assistant:two,=c4|user:<c4|assistant:done");
    expect_text(db, t.sid, "look2", 0);
    expect_text(db, t.sid, "turn_edits", 0);
    /* Reasoning is not replayed beside a subset of its calls; the untouched
     * iteration and the answer keep theirs. */
    expect_body(db, t.sid, O, "$.messages[3].content", "one");
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details", NULL);
    expect_body(db, t.sid, O, "$.messages[6].reasoning_details[0].text", "why2");
    expect_body(db, t.sid, O, "$.messages[8].reasoning_details[0].text", "why3");
    expect_text(db, t.sid, "why1", 0);

    /* The session goes on: the next message opens a new turn under the same
     * leaf. */
    int64_t u3 = add_user(db, t.sid, "next");
    expect_q(db, "1", "SELECT turn_id=3 AND parent_id=%lld FROM entries WHERE id=%lld",
             (long long)t.fin, (long long)u3);
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c3|user:<c1,<c3"
        "|assistant:two,=c4|user:<c4|assistant:done*|user:next");


    /* Its only call gone, an iteration leaves whole: text and reasoning too;
     * the batch nobody touched still replays its reasoning. */
    T w = seed_turn(db, "dropit", 0);
    apply_ok(db, &w, E(DROP("c4")));
    BRANCH(db, w.sid, w.u0, w.a0, w.u, w.rs1, w.a1, w.c1, w.c2, w.c3, w.r2, w.r1, w.r3,
           w.rs3, w.fin);
    REC_IDS(db, &w, ".left_branch", w.rs2, w.a2, w.c4, w.r4);
    expect_shape(db, w.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c2,=c3|user:<c2,<c1,<c3"
        "|assistant:done");
    expect_text(db, w.sid, "two", 0);
    expect_text(db, w.sid, "why2", 0);
    expect_body(db, w.sid, O, "$.messages[3].reasoning_details[0].text", "why1");

    /* Both droppable calls of a batch gone, the protected one staying, and
     * the other iteration emptied, in one reply. */
    T b = seed_turn(db, "dropboth", 0);
    apply_ok(db, &b, E(DROP("c4") "," DROP("c2") "," DROP("c1")));
    expect_shape(db, b.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c3|tool:r3,<c3|assistant:done");
    expect_shape(db, b.sid, G,
        "user:hi|model:hello|user:go|model:one,=act|user:<act|model:done");
    expect_body(db, b.sid, O, "$.messages[3].reasoning_details", NULL);
    REC_IDS(db, &b, ".left_branch", b.rs1, b.c1, b.c2, b.r2, b.r1, b.rs2, b.a2, b.c4, b.r4);
    free(branch(db, b.sid));

    db_close(db); test_db_clean(DB_PATH);
}

static void test_replace(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "replace", 0);
    run(db, "UPDATE entries SET content='ORIGINAL-ONE' WHERE id=%lld", (long long)t.r1);
    run(db, "UPDATE entries SET content='ORIGINAL-THREE', is_error=1 WHERE id=%lld",
        (long long)t.r3);
    run(db, "UPDATE entries SET content='ORIGINAL-FOUR', network_hosts='[\"example.com\"]',"
            " fence_id='f00d' WHERE id=%lld", (long long)t.r4);
    char *r1_before = q(db, "SELECT json_array(parent_id, original_parent_id, content,"
                            " token_estimate) FROM entries WHERE id=%lld", (long long)t.r1);

    /* c3 is protected: its result's text may still be replaced. The c1
     * replacement is longer than the original and is applied all the same. */
    apply_ok(db, &t, E(
        "{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"REPL-ONE, which is much longer"
        " than the result it stands in for\"},"
        "{\"op\":\"replace\",\"call_id\":\"c3\",\"text\":\"REPL-THREE\"},"
        "{\"op\":\"replace\",\"call_id\":\"c4\",\"text\":\"REPL-FOUR\"}"));
    int64_t n1 = leaf_rec_id(db, t.sid, ".replaced[0].replacement");
    int64_t n3 = leaf_rec_id(db, t.sid, ".replaced[1].replacement");
    int64_t n4 = leaf_rec_id(db, t.sid, ".replaced[2].replacement");
    assert(n1 > t.fin && n3 > n1 && n4 > n3);

    /* New entries in the originals' places. */
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, n1, n3, t.rs2,
           t.a2, t.c4, n4, t.rs3, t.fin);
    /* The replacement is the original in every respect but text and size. */
    expect_q(db, "1",
        "SELECT n.type=o.type AND n.role=3 AND n.turn_id=o.turn_id"
        " AND n.iteration_id=o.iteration_id AND n.tool_call_id=o.tool_call_id"
        " AND n.tool_name=o.tool_name AND n.is_error=o.is_error"
        " AND n.network_hosts IS o.network_hosts AND n.fence_id IS o.fence_id"
        " AND n.content_bytes=length(CAST(n.content AS BLOB))"
        " AND n.token_estimate>o.token_estimate"
        " FROM entries n, entries o WHERE n.id=%lld AND o.id=%lld", (long long)n1,
        (long long)t.r1);
    expect_q(db, "1|c3|REPL-THREE", "SELECT is_error || '|' || tool_call_id || '|' || content"
                                    " FROM entries WHERE id=%lld", (long long)n3);
    expect_q(db, "[\"example.com\"]|f00d|2", "SELECT network_hosts || '|' || fence_id || '|' ||"
                                             " turn_id FROM entries WHERE id=%lld", (long long)n4);
    /* The original stays in the table, untouched, off the branch. */
    expect_q(db, r1_before, "SELECT json_array(parent_id, original_parent_id, content,"
                            " token_estimate) FROM entries WHERE id=%lld", (long long)t.r1);
    free(r1_before);

    /* The record: originals off the branch in chain order, pairs in edit
     * order. */
    REC_IDS(db, &t, ".left_branch", t.r1, t.r3, t.r4);
    char want[256];
    snprintf(want, sizeof(want),
             "[{\"original\":%lld,\"replacement\":%lld},{\"original\":%lld,\"replacement\":%lld},"
             "{\"original\":%lld,\"replacement\":%lld}]", (long long)t.r1, (long long)n1,
             (long long)t.r3, (long long)n3, (long long)t.r4, (long long)n4);
    expect_rec(db, &t, ".replaced", want);

    /* On the wire: every call still answered, by the replacement. */
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c2,=c3|user:<c2,<c1,<c3"
        "|assistant:two,=c4|user:<c4|assistant:done");
    expect_text(db, t.sid, "REPL-ONE, which is much longer", 1);
    expect_text(db, t.sid, "REPL-THREE", 1);
    expect_text(db, t.sid, "ORIGINAL-", 0);
    expect_body(db, t.sid, O, "$.messages[5].tool_call_id", "c1");
    expect_body(db, t.sid, G, "$.contents[5].parts[0].functionResponse.name", "look");
    expect_body(db, t.sid, A, "$.messages[4].content[1].tool_use_id", "c1");
    /* The network result keeps its untrusted-content fence. */
    expect_body(db, t.sid, O, "$.messages[8].content",
        "<UNTRUSTED_EXTERNAL_CONTENT id=\"f00d\">\nContacted hosts: [\"example.com\"]\n---\n"
        "REPL-FOUR\n</UNTRUSTED_EXTERNAL_CONTENT id=\"f00d\">");
    /* Nothing was dropped, so every iteration still replays its reasoning. */
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details[0].text", "why1");
    expect_body(db, t.sid, O, "$.messages[7].reasoning_details[0].text", "why2");

    db_close(db); test_db_clean(DB_PATH);
}

static void test_notes(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "notes", SEED_NOTE);
    /* Two notes at the default place, one before the second iteration; a
     * zero-width space is stripped on the way in. */
    char reply[512];
    snprintf(reply, sizeof(reply), E(
        "{\"op\":\"note\",\"text\":\"fir\xE2\x80\x8Bst\"},"
        "{\"op\":\"note\",\"text\":\"before two\",\"before\":%lld},"
        "{\"op\":\"note\",\"text\":\"second\"}"), (long long)t.rs2);
    apply_ok(db, &t, reply);
    int64_t n1 = leaf_rec_id(db, t.sid, ".notes[0]");
    int64_t n2 = leaf_rec_id(db, t.sid, ".notes[1]");
    int64_t n3 = leaf_rec_id(db, t.sid, ".notes[2]");
    /* Written in chain order, recorded in edit order. */
    assert(n2 > t.fin && n1 > n2 && n3 > n1);

    /* Default: before the answer's iteration (its reasoning included), in
     * edit order. Explicit: right before the named iteration's first row,
     * after the system entry already there. */
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, t.r1, t.r3,
           t.sys, n2, t.rs2, t.a2, t.c4, t.r4, n1, n3, t.rs3, t.fin);
    expect_q(db, "system|0|0|2|first|hook:rev",
             "SELECT concat_ws('|', type, role, iteration_id, turn_id, content,"
             " json_extract(data,'$.source')) FROM entries WHERE id=%lld", (long long)n1);
    expect_q(db, "hook:rev|hook:rev", "SELECT group_concat(json_extract(data,'$.source'),'|')"
             " FROM entries WHERE id IN (%lld,%lld)", (long long)n2, (long long)n3);
    REC_IDS(db, &t, ".notes", n1, n2, n3);
    expect_rec(db, &t, ".left_branch", "[]");
    expect_rec(db, &t, ".replaced", "[]");

    /* On the wire: the hook's notes are labelled as the extension's, the
     * ordinary system entry keeps '[system] '. Nothing dropped, so reasoning
     * replays everywhere. */
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c2,=c3|tool:r2,<c2|tool:r1,<c1"
        "|tool:r3,<c3|user:[system] sys mid|user:" NOTE_TAG "before two"
        "|assistant:two,=c4|tool:r4,<c4|user:" NOTE_TAG "first|user:" NOTE_TAG "second"
        "|assistant:done");
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c2,=c3"
        "|user:<c2,<c1,<c3|user:[system] sys mid|user:" NOTE_TAG "before two"
        "|assistant:two,=c4|user:<c4|user:" NOTE_TAG "first|user:" NOTE_TAG "second"
        "|assistant:done");
    expect_text(db, t.sid, "[system] " NOTE_TAG, 0);
    expect_text(db, t.sid, "[system] first", 0);

    /* "before" naming the answer — the id the input gives as final.entry_id
     * — places the note before the answer's reasoning, as the default does. */
    snprintf(reply, sizeof(reply), E("{\"op\":\"note\",\"text\":\"third\",\"before\":%lld}"),
             (long long)t.fin);
    apply_ok(db, &t, reply);
    int64_t n4 = leaf_rec_id(db, t.sid, ".notes[0]");
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, t.r1, t.r3,
           t.sys, n2, t.rs2, t.a2, t.c4, t.r4, n1, n3, n4, t.rs3, t.fin);
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details[0].text", "why1");
    expect_body(db, t.sid, O, "$.messages[9].reasoning_details[0].text", "why2");
    expect_body(db, t.sid, O, "$.messages[14].reasoning_details[0].text", "why3");

    /* A note on its own, on an answer without a reasoning entry: it sits
     * directly under the answer. */
    int64_t sid = session_create(db, "plain", "default", -1, 0);
    int64_t u = add_user(db, sid, "go");
    int64_t it = db_next_iteration_id(db, sid);
    int64_t c = add_call(db, sid, it, "k1", "look", "{}", "done");
    int64_t r = add_typed(db, sid, it, "tool_result", "res", "k1", "look");
    int64_t fin = add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message",
                            "done", NULL, NULL);
    assert(turn_edit_apply(db, sid, E("{\"op\":\"note\",\"text\":\"n\"}"), "hook:x", NULL)
           == TURN_EDIT_APPLIED);
    int64_t n = db_scalar_i64(db, "SELECT MAX(id) FROM entries WHERE session_id=?", sid, -1);
    char want[128], *got = branch(db, sid);
    snprintf(want, sizeof(want), "%lld,%lld,%lld,%lld,%lld", (long long)u, (long long)c,
             (long long)r, (long long)n, (long long)fin);
    assert(strcmp(got, want) == 0);
    free(got);
    expect_shape(db, sid, O, "user:go|assistant:=k1|tool:res,<k1"
                             "|user:[note from extension x] n|assistant:done");
    /* A note before an iteration whose first row is a call. */
    snprintf(want, sizeof(want), E("{\"op\":\"note\",\"text\":\"m\",\"before\":%lld}"),
             (long long)c);
    assert(turn_edit_apply(db, sid, want, "hook:x", NULL) == TURN_EDIT_APPLIED);
    expect_shape(db, sid, O, "user:go|user:[note from extension x] m|assistant:=k1"
                             "|tool:res,<k1|user:[note from extension x] n|assistant:done");

    db_close(db); test_db_clean(DB_PATH);
}

/* The record across two applies, and the tree under it. */
static void test_record(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "record", 0);
    run(db, "UPDATE entries SET data='{\"keep\":\"me\",\"n\":[1,2]}' WHERE id=%lld",
        (long long)t.fin);
    int64_t before1 = branch_tokens(db, &t);

    apply_ok(db, &t, "{\"edits\":[" DROP("c4") "],"
                     "\"record\":{\"score\":3,\"why\":[\"long grep\"]}}");
    int64_t after1 = branch_tokens(db, &t);
    /* rs3 was re-pointed past the emptied iteration. */
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.r4, (long long)t.r3, (long long)t.rs3);

    /* The next hook sees the turn as the first left it. */
    char *in = input_turn(db, t.sid);
    assert(in);
    expect_json(db, in, "json_array_length(?1,'$.iterations')", "1");
    free(in);

    apply_as(db, &t, "hook:two", E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"short\"},"
                                   "{\"op\":\"note\",\"text\":\"second pass\"}"));
    int64_t n1 = leaf_rec_id(db, t.sid, ".replaced[0].replacement");
    int64_t note = leaf_rec_id(db, t.sid, ".notes[0]");
    assert(n1 > t.fin && note > n1);

    /* Existing keys kept; two elements, the script's record riding on the
     * first only. */
    expect_q(db, "me|[1,2]|2", "SELECT json_extract(data,'$.keep') || '|' ||"
             " json_extract(data,'$.n') || '|' || json_array_length(data,'$.turn_edits')"
             " FROM entries WHERE id=%lld", (long long)t.fin);
    expect_q(db, "{\"score\":3,\"why\":[\"long grep\"]}",
             "SELECT json_extract(data,'$.turn_edits[0].record') FROM entries WHERE id=%lld",
             (long long)t.fin);
    expect_q(db, "hook:rev|hook:two", "SELECT json_extract(data,'$.turn_edits[0].by') || '|' ||"
             " json_extract(data,'$.turn_edits[1].by') FROM entries WHERE id=%lld",
             (long long)t.fin);
    expect_q(db, NULL, "SELECT json_type(data,'$.turn_edits[1].record') FROM entries"
                       " WHERE id=%lld", (long long)t.fin);
    expect_q(db, "1", "SELECT json_extract(data,'$.turn_edits[0].tokens.before')=%lld"
                      " AND json_extract(data,'$.turn_edits[0].tokens.after')=%lld"
                      " AND json_extract(data,'$.turn_edits[1].tokens.before')=%lld"
                      " AND json_extract(data,'$.turn_edits[1].tokens.after')=%lld"
                      " FROM entries WHERE id=%lld", (long long)before1, (long long)after1,
             (long long)after1, (long long)branch_tokens(db, &t), (long long)t.fin);
    REC_IDS(db, &t, ".left_branch", t.r1);
    REC_IDS(db, &t, ".notes", note);
    /* The record never reaches the model. */
    expect_text(db, t.sid, "long grep", 0);
    expect_text(db, t.sid, "turn_edits", 0);

    /* rs3 re-pointed again, under the note: its first original parent wins. */
    expect_q(db, "1", "SELECT original_parent_id=%lld AND parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)t.r4, (long long)note, (long long)t.rs3);
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, n1, t.r3, note,
           t.rs3, t.fin);
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c2,=c3|tool:r2,<c2|tool:short,<c1"
        "|tool:r3,<c3|user:[note from extension two] second pass|assistant:done");

    /* The next user message opens a new turn under the same leaf. */
    int64_t u3 = add_user(db, t.sid, "next");
    expect_q(db, "1", "SELECT turn_id=3 AND parent_id=%lld FROM entries WHERE id=%lld",
             (long long)t.fin, (long long)u3);
    free(branch(db, t.sid));

    db_close(db); test_db_clean(DB_PATH);
}

/* One finished turn with no tools: a user message and its answer. */
static int64_t add_plain_turn(sqlite3 *db, int64_t sid, const char *ask, const char *answer) {
    add_user(db, sid, ask);
    return add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message", answer,
                     NULL, NULL);
}

/* `history`: the rest of the active branch, oldest first, each earlier turn
 * in the finished turn's own shape — and none of it editable. */
static void test_history(void) {
    sqlite3 *db = open_seeded();
    #define IN(path, want) expect_at(db, in, path, want)
    #define LEN(path, want) expect_json(db, in, "json_array_length(?1,'" path "')", want)

    /* A session's first turn has nothing before it. */
    int64_t one = session_create(db, "one", "default", -1, 0);
    add_plain_turn(db, one, "hi", "hello");
    char *in = input_all(db, one);
    assert(in);
    expect_json(db, in, "json_type(?1,'$.history')", "array");
    LEN("$.history", "0");
    IN("$.final.text", "hello");
    free(in);

    /* Three turns. The first and the third each made a call the provider
     * named "c1"; the first also made one only it has. */
    int64_t sid = session_create(db, "three", "default", -1, 0);
    int64_t u1 = add_user(db, sid, "first");
    int64_t it1 = db_next_iteration_id(db, sid);
    int64_t a1 = add_typed(db, sid, it1, "assistant_message", "looking", NULL, NULL);
    add_call(db, sid, it1, "c1", "look", "{\"n\":1}", "done");
    add_call(db, sid, it1, "only1", "look", "{}", "done");
    int64_t old_r = add_typed(db, sid, it1, "tool_result", "OLD", "c1", "look");
    add_typed(db, sid, it1, "tool_result", "o1", "only1", "look");
    int64_t f1 = add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message",
                           "answer one", NULL, NULL);
    /* A turn that ended on a provider error: the request never carries that
     * entry again, so history shows the turn without it and with no answer. */
    int64_t u2 = add_user(db, sid, "second");
    int64_t boom = add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message",
                             "provider failed [resp #9]", NULL, NULL);
    run(db, "UPDATE entries SET stop_reason=%d WHERE id=%lld", (int)STOP_REASON_ERROR,
        (long long)boom);
    int64_t u3 = add_user(db, sid, "third");
    int64_t it3 = db_next_iteration_id(db, sid);
    int64_t a3 = add_typed(db, sid, it3, "assistant_message", "again", NULL, NULL);
    int64_t c3 = add_call(db, sid, it3, "c1", "look", "{\"n\":3}", "done");
    int64_t new_r = add_typed(db, sid, it3, "tool_result", "NEW", "c1", "look");
    int64_t f3 = add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message",
                           "answer three", NULL, NULL);

    in = input_all(db, sid);
    assert(in);
    /* The finished turn is at the top level, as it always was. */
    IN("$.turn_id", "3");
    expect_id_at(db, in, "$.user_messages[0].entry_id", u3);
    LEN("$.iterations", "1");
    expect_id_at(db, in, "$.iterations[0].entry_id", a3);
    /* A call's result is its own turn's, whatever an earlier turn called its
     * calls. */
    expect_id_at(db, in, "$.iterations[0].calls[0].result.entry_id", new_r);
    IN("$.iterations[0].calls[0].result.text", "NEW");
    expect_id_at(db, in, "$.final.entry_id", f3);
    expect_id_at(db, in, "$.tokens.turn", db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=? AND turn_id=3", sid, -1));

    LEN("$.history", "2");
    /* An earlier turn has the finished turn's shape (no event or input). */
    expect_json(db, in, "(SELECT group_concat(key, ',') FROM json_each(?1,'$.history[0]'))",
                "turn_id,tokens,user_messages,notes,iterations,final");
    IN("$.history[0].turn_id", "1");
    expect_id_at(db, in, "$.history[0].tokens.turn", db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=? AND turn_id=1", sid, -1));
    expect_id_at(db, in, "$.history[0].user_messages[0].entry_id", u1);
    expect_id_at(db, in, "$.history[0].iterations[0].entry_id", a1);
    IN("$.history[0].iterations[0].calls[1].call_id", "only1");
    /* Its "c1" result is its own, not the newest turn's. */
    expect_id_at(db, in, "$.history[0].iterations[0].calls[0].result.entry_id", old_r);
    expect_id_at(db, in, "$.history[0].final.entry_id", f1);
    IN("$.history[1].turn_id", "2");
    expect_id_at(db, in, "$.history[1].user_messages[0].entry_id", u2);
    LEN("$.history[1].iterations", "0");
    expect_json(db, in, "json_type(?1,'$.history[1].final')", "null");
    free(in);

    /* A call id names a call of the newest turn unless turn_id says which:
     * a call only an earlier turn made is not there to name without it. A
     * user message is no place for a note, in any turn. */
    char r[160];
    const TurnEditResult NO = TURN_EDIT_REJECTED;
    expect_untouched(db, sid, E(DROP("only1")), NO, "not a tool call of this turn");
    expect_untouched(db, sid, E("{\"op\":\"replace\",\"call_id\":\"only1\",\"text\":\"x\"}"),
                     NO, "replace: \"only1\" is not a tool call of this turn");
    expect_untouched(db, sid, E("{\"op\":\"drop\",\"call_id\":\"only1\",\"turn_id\":2}"), NO,
                     "turn 2");
    int64_t not_here[] = { u1, u2 };
    for (size_t k = 0; k < sizeof(not_here) / sizeof(not_here[0]); k++) {
        snprintf(r, sizeof(r), E("{\"op\":\"note\",\"text\":\"n\",\"before\":%lld}"),
                 (long long)not_here[k]);
        expect_untouched(db, sid, r, NO, "first entry");
    }
    /* A call id both turns used names the finished turn's call, and only
     * that one leaves. */
    assert(turn_edit_apply(db, sid, E(DROP("c1")), LABEL, NULL) == TURN_EDIT_APPLIED);
    expect_q(db, "0", "SELECT COUNT(*) FROM entries WHERE turn_id < 3 AND session_id=%lld"
                      " AND original_parent_id IS NOT NULL", (long long)sid);
    char *b = branch(db, sid);
    snprintf(r, sizeof(r), ",%lld,", (long long)old_r);
    assert(strstr(b, r));
    snprintf(r, sizeof(r), ",%lld,", (long long)c3);
    assert(!strstr(b, r));
    snprintf(r, sizeof(r), ",%lld,", (long long)new_r);
    assert(!strstr(b, r));
    free(b);
    in = input_all(db, sid);
    assert(in);
    LEN("$.iterations", "0");
    IN("$.history[0].iterations[0].calls[0].result.text", "OLD");
    free(in);

    /* An edited turn, one turn later: history is the branch — what the agent
     * now sees — not the entries as first written. */
    T t = seed_turn(db, "edited", 0);
    apply_ok(db, &t, E(DROP("c2") ","
        "{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"SHORTER\"},"
        "{\"op\":\"note\",\"text\":\"tidied\"}"));
    int64_t f_next = add_plain_turn(db, t.sid, "next", "sure");
    in = input_all(db, t.sid);
    assert(in);
    expect_id_at(db, in, "$.final.entry_id", f_next);
    IN("$.history[1].turn_id", "2");
    expect_json(db, in,
        "(SELECT group_concat(json_extract(c.value,'$.call_id'), ',')"
        "  FROM json_each(?1,'$.history[1].iterations') i, json_each(i.value,'$.calls') c)",
        "c1,c3,c4");
    IN("$.history[1].iterations[0].calls[0].result.text", "SHORTER");
    /* A kept subset of a batch lost its reasoning row: the iteration now
     * opens at its assistant message. */
    expect_id_at(db, in, "$.history[1].iterations[0].entry_id", t.a1);
    IN("$.history[1].notes[0].text", "tidied");
    expect_id_at(db, in, "$.history[1].tokens.turn", turn_tokens(db, &t));
    free(in);

    #undef IN
    #undef LEN
    db_close(db); test_db_clean(DB_PATH);
}

/* `n` copies of `c`, NUL-terminated (caller frees). */
static char *repeat(char c, int n) {
    char *t = malloc((size_t)n + 1);
    assert(t);
    memset(t, c, (size_t)n);
    t[n] = '\0';
    return t;
}

/* A turn with one call of `look` answered by `result`, then its answer. */
static void add_call_turn(sqlite3 *db, int64_t sid, const char *ask, const char *result,
                          const char *answer) {
    add_user(db, sid, ask);
    int64_t it = db_next_iteration_id(db, sid);
    add_typed(db, sid, it, "assistant_message", "looking", NULL, NULL);
    add_call(db, sid, it, "c1", "look", "{\"q\":1}", "done");
    add_typed(db, sid, it, "tool_result", result, "c1", "look");
    add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message", answer, NULL, NULL);
}

/* A response that kept a reasoning summary (agent_models.reasoning_display)
 * sends it as its iteration's `reasoning` — the answer's too — and never the
 * replay blob or signature. It is priced as written: an earlier turn whose
 * summary does not fit is left out whole, and the summary of a response that
 * stopped on an error, which is not sent, costs nothing. */
static void test_input_reasoning(void) {
    sqlite3 *db = open_seeded();
    #define IN(path, want) expect_at(db, in, path, want)
    T t = seed_turn(db, "reasoning", 0);
    run(db, "UPDATE entries SET content='I check /a first.' WHERE id=%lld", (long long)t.rs1);
    run(db, "UPDATE entries SET content='All three agree.' WHERE id=%lld", (long long)t.rs3);
    char *in = input_turn(db, t.sid);
    assert(in);
    IN("$.iterations[0].reasoning", "I check /a first.");
    expect_json(db, in, "json_type(?1,'$.iterations[1].reasoning')", NULL);   /* none kept */
    IN("$.final.reasoning", "All three agree.");
    IN("$.final.text", "done");
    expect_json(db, in, "instr(?1,'why1') + instr(?1,'why3') + instr(?1,'reasoning_meta')", "0");
    /* The tokens are the entries' as before: the summary's text changes no count. */
    expect_id_at(db, in, "$.tokens.turn", db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=? AND turn_id=2", t.sid, -1));
    free(in);

    /* Earlier turns: one whose answer carries a long summary, then one whose
     * response stopped on an error with a longer one, then the newest. */
    int64_t sid = session_create(db, "reasoning-history", "default", -1, 0);
    char *big = repeat('s', 3000), *bigger = repeat('e', 6000);
    add_user(db, sid, "q1");
    int64_t it = db_next_iteration_id(db, sid);
    add_typed(db, sid, it, "reasoning", big, NULL, NULL);
    add_typed(db, sid, it, "assistant_message", "answer 1", NULL, NULL);
    add_user(db, sid, "q2");
    it = db_next_iteration_id(db, sid);
    add_typed(db, sid, it, "reasoning", bigger, NULL, NULL);
    int64_t err = add_typed(db, sid, it, "assistant_message", "partial", NULL, NULL);
    run(db, "UPDATE entries SET stop_reason=4 WHERE id=%lld", (long long)err);
    add_plain_turn(db, sid, "q3", "answer 3");

    in = input_as(db, sid, TURN_EDIT_HISTORY_ALL, 1, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    assert(in);
    IN("$.history[0].final.reasoning", big);
    IN("$.history_omitted", "0");
    expect_json(db, in, "instr(?1,'eeee')", "0");     /* the errored response's: not sent */
    int whole = (int)strlen(in);
    free(in);
    /* Its size, plus the room kept for the splice: everything, though the
     * unsent summary alone would not have fitted. Less than the sent one's
     * size: the oldest turn, summary and all, goes. */
    in = input_as(db, sid, TURN_EDIT_HISTORY_ALL, 1, whole + 64, NULL);
    assert(in);
    IN("$.history_omitted", "0");
    expect_json(db, in, "json_array_length(?1,'$.history')", "2");
    free(in);
    in = input_as(db, sid, TURN_EDIT_HISTORY_ALL, 1, whole - 100, NULL);
    assert(in);
    IN("$.history_omitted", "1");
    expect_json(db, in, "instr(?1,'ssss')", "0");
    expect_json(db, in, "json_array_length(?1,'$.history')", "1");
    free(in);
    free(big); free(bigger);
    #undef IN
    db_close(db); test_db_clean(DB_PATH);
}

/* What a hook asks to be sent (hooks.input): how many earlier turns, with or
 * without their results' text, within max_bytes — the finished turn always
 * whole, the oldest history left out first, and the input saying what it was
 * given. */
static void test_input_options(void) {
    sqlite3 *db = open_seeded();
    #define IN(path, want) expect_at(db, in, path, want)
    #define LEN(path, want) expect_json(db, in, "json_array_length(?1,'" path "')", want)
    #define TURN_IDS(want) expect_json(db, in, "(SELECT group_concat(COALESCE(" \
        "json_extract(value,'$.turn_id'), 'S'), ',') FROM json_each(?1,'$.history'))", want)

    int64_t sid = session_create(db, "opts", "default", -1, 0);
    add_call_turn(db, sid, "q1", "old result 1", "answer 1");
    add_call_turn(db, sid, "q2", "old result 2", "answer 2");
    add_call_turn(db, sid, "q3", "old result 3", "answer 3");
    add_call_turn(db, sid, "q4", "new result", "answer 4");

    /* The last N turns, oldest first; more than there are is all of them. */
    char *in = input_as(db, sid, 2, 1, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    assert(in);
    IN("$.turn_id", "4");
    TURN_IDS("2,3");
    IN("$.history_omitted", "0");
    IN("$.input.history", "2");
    free(in);
    in = input_as(db, sid, 1, 1, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    TURN_IDS("3");
    free(in);
    in = input_as(db, sid, 10, 1, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    TURN_IDS("1,2,3");
    IN("$.input.history", "10");
    IN("$.history_omitted", "0");
    free(in);
    in = input_all(db, sid);
    TURN_IDS("1,2,3");
    IN("$.input.history", "all");
    free(in);

    /* history_results false: earlier calls keep their arguments and their
     * result's entry_id and tokens, and the result has no text. The finished
     * turn's results are always whole. */
    in = input_as(db, sid, TURN_EDIT_HISTORY_ALL, 0, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    assert(in);
    IN("$.input.history_results", "0");
    expect_json(db, in, "json_type(?1,'$.input.history_results')", "false");
    expect_json(db, in, "(SELECT group_concat(key, ',') FROM"
                        " json_each(?1,'$.history[0].iterations[0].calls[0].result'))",
                "entry_id,tokens");
    IN("$.history[0].iterations[0].calls[0].args.q", "1");
    IN("$.history[0].final.text", "answer 1");
    expect_id_at(db, in, "$.history[0].iterations[0].calls[0].result.tokens",
                 db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE content='old result 1'"
                                   " AND session_id=?", sid, -1));
    expect_json(db, in, "instr(?1,'old result')", "0");
    IN("$.iterations[0].calls[0].result.text", "new result");
    free(in);

    /* Fitting to max_bytes. The branch: what compaction left of turn 1 (one
     * long user message), the summary, three turns of ~1 KB (turn ids 3-5), the
     * finished turn. What decides is the text as written out: `alone` is the
     * input with no history, each earlier unit costs its own length and a
     * comma, and SPLICE bytes are kept for the history's frame. */
    char *k2 = repeat('k', 2000), *x = repeat('x', 1000), *sum = repeat('s', 300);
    int64_t cs = session_create(db, "fit", "default", -1, 0);
    int64_t u0 = add_user(db, cs, k2);
    add_typed(db, cs, db_next_iteration_id(db, cs), "assistant_message", "a1", NULL, NULL);
    add_plain_turn(db, cs, "q2", "a2");
    int64_t u4 = add_user(db, cs, "q4");
    add_typed(db, cs, db_next_iteration_id(db, cs), "assistant_message", x, NULL, NULL);
    add_plain_turn(db, cs, "q5", x);
    add_plain_turn(db, cs, "q6", x);
    add_plain_turn(db, cs, "go", "done");
    assert(test_legacy_compact(db, cs, u0, u4, sum) > 0);
    const int SPLICE = 64;

    in = input_all(db, cs);
    TURN_IDS("1,S,3,4,5");
    /* Each unit's length as written out, history[0] (turn 1) to [4] (turn 5). */
    int unit[5];
    for (int i = 0; i < 5; i++) {
        char path[32];
        snprintf(path, sizeof(path), "$.history[%d]", i);
        unit[i] = (int)json_len_at(db, in, path);
        assert(unit[i] > 0);
    }
    free(in);
    in = input_as(db, cs, 0, 1, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    const int alone = (int)strlen(in);
    free(in);
    #define ROOM(a, b) (alone + SPLICE + (a) + (b))
    /* Room for the three turns and the summary, not for the long remnant of
     * turn 1, which is older than the summary: it goes first. */
    int max = ROOM(unit[1] + unit[2] + unit[3] + unit[4] + 4, 40);
    in = input_as(db, cs, TURN_EDIT_HISTORY_ALL, 1, max, NULL);
    assert(in && (int)strlen(in) <= max);
    TURN_IDS("S,3,4,5");
    IN("$.history_omitted", "1");
    IN("$.input.history", "all");
    free(in);
    /* Room for two turns: the summary goes with everything older than them,
     * and only turns are counted as left out. */
    max = ROOM(unit[3] + unit[4] + 2, 40);
    in = input_as(db, cs, TURN_EDIT_HISTORY_ALL, 1, max, NULL);
    assert(in && (int)strlen(in) <= max);
    TURN_IDS("4,5");
    IN("$.history_omitted", "2");
    free(in);
    /* A count is the most that is sent: within it, the same trimming. */
    in = input_as(db, cs, 2, 1, max, NULL);
    TURN_IDS("4,5");
    IN("$.history_omitted", "0");
    free(in);
    in = input_as(db, cs, 3, 1, max, NULL);
    TURN_IDS("4,5");
    IN("$.history_omitted", "1");
    free(in);
    /* With room, a count reaches a summary only past a turn older than it. */
    in = input_as(db, cs, 3, 1, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    TURN_IDS("3,4,5");
    free(in);
    in = input_as(db, cs, 4, 1, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    TURN_IDS("1,S,3,4,5");
    free(in);
    /* Leaving results out makes room: here the remnant has none, so the
     * same budget sends the same history. */
    in = input_as(db, cs, TURN_EDIT_HISTORY_ALL, 0, max, NULL);
    TURN_IDS("4,5");
    free(in);
    /* No room for any history: the turn alone, every earlier turn counted. */
    int too_big = -1;
    max = ROOM(0, 10);
    in = input_as(db, cs, TURN_EDIT_HISTORY_ALL, 1, max, &too_big);
    assert(in && (int)strlen(in) <= max && too_big == 0);
    LEN("$.history", "0");
    IN("$.history_omitted", "4");
    IN("$.final.text", "done");
    free(in);
    /* The finished turn alone over the limit: nothing is built, and the
     * caller is told why. */
    assert(input_as(db, cs, 0, 1, alone - 40, &too_big) == NULL && too_big == 1);
    assert(input_as(db, cs, TURN_EDIT_HISTORY_ALL, 1, alone - 40, &too_big) == NULL &&
           too_big == 1);
    /* No finished turn is not "too big". */
    add_user(db, cs, "pending");
    assert(input_as(db, cs, 0, 1, TURN_EDIT_INPUT_MAX_BYTES, &too_big) == NULL &&
           too_big == 0);
    free(k2); free(x); free(sum);

    /* max_bytes bounds the text handed over, not what is stored. Control
     * characters are written out six bytes each (\u0001): the stored length
     * admits the earlier turn, the text does not fit, and it is left out. */
    int64_t es = session_create(db, "escape", "default", -1, 0);
    int64_t ea = add_plain_turn(db, es, "q1", "a1");
    run(db, "UPDATE entries SET content=replace(printf('%%.*c',600,'x'),'x',char(1)),"
            " content_bytes=600 WHERE id=%lld", (long long)ea);
    int64_t eb = add_plain_turn(db, es, "go", "done");
    in = input_as(db, es, 0, 1, TURN_EDIT_INPUT_MAX_BYTES, NULL);
    const int es_alone = (int)strlen(in);
    free(in);
    max = es_alone + SPLICE + 600 + 400;
    in = input_as(db, es, TURN_EDIT_HISTORY_ALL, 1, max, &too_big);
    assert(in && (int)strlen(in) <= max && too_big == 0);
    LEN("$.history", "0");
    IN("$.history_omitted", "1");
    free(in);
    in = input_all(db, es);
    assert(strlen(in) > (size_t)max);
    LEN("$.history", "1");
    free(in);
    /* The same in the finished turn: it does not fit, and is not sent. */
    run(db, "UPDATE entries SET content=replace(printf('%%.*c',600,'x'),'x',char(1)),"
            " content_bytes=600 WHERE id=%lld", (long long)eb);
    assert(input_as(db, es, 0, 1, 1500, &too_big) == NULL && too_big == 1);
    #undef ROOM

    #undef IN
    #undef LEN
    #undef TURN_IDS
    db_close(db); test_db_clean(DB_PATH);
}

/* Stage 0: a system note after a turn's answer (the streak guard's, which
 * hangs off the leaf and so inherits the turn) does not hide the answer —
 * the turn's end is its newest row that is not a note. */
static void test_note_after_answer(void) {
    sqlite3 *db = open_seeded();
    #define IN(path, want) expect_at(db, in, path, want)
    int64_t sid = session_create(db, "streak", "default", -1, 0);
    int64_t f1 = add_plain_turn(db, sid, "q1", "answer one");
    int64_t note = add_system(db, sid, "autonomous-turn limit reached");
    assert(db_scalar_i64(db, "SELECT turn_id FROM entries WHERE id=?", note, -1) ==
           db_scalar_i64(db, "SELECT turn_id FROM entries WHERE id=?", f1, -1));
    /* As history, one turn later. */
    int64_t f2 = add_plain_turn(db, sid, "q2", "answer two");
    char *in = input_all(db, sid);
    assert(in);
    expect_id_at(db, in, "$.final.entry_id", f2);
    expect_id_at(db, in, "$.history[0].final.entry_id", f1);
    IN("$.history[0].final.text", "answer one");
    expect_id_at(db, in, "$.history[0].notes[0].entry_id", note);
    expect_json(db, in, "json_type(?1,'$.history[0].notes[0].before_iteration')", "null");
    free(in);
    /* As the newest turn, to an event that does not need the answer to be
     * the leaf (a stop event never fires on such a turn). */
    add_plain_turn(db, sid, "q3", "answer three");
    int64_t f3 = db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", sid, -1);
    add_system(db, sid, "another note");
    TurnEditInputSpec spec = TURN_EDIT_INPUT_DEFAULTS;
    TurnEditInputFor ct = { .event = "ContextThreshold", .need_answer = 0 };
    in = turn_edit_input(db, sid, &spec, &ct, NULL);
    assert(in);
    expect_id_at(db, in, "$.final.entry_id", f3);
    free(in);
    #undef IN
    db_close(db); test_db_clean(DB_PATH);
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
    char *in = input_turn(db, sid);
    assert(in);
    expect_at(db, in, "$.iterations[0].calls[0].result.text", "nothing there");
    free(in);
    int64_t cmp = test_legacy_compact(db, sid, u0, u, "sum");
    assert(cmp > 0);

    assert(turn_edit_apply(db, sid, E(DROP("k1")), LABEL, NULL) == TURN_EDIT_APPLIED);
    char want[96];
    snprintf(want, sizeof(want), "%lld,%lld,%lld,%lld", (long long)u0, (long long)cmp,
             (long long)u, (long long)fin);
    char *got = branch(db, sid);
    assert(strcmp(got, want) == 0);
    free(got);
    expect_q(db, "1", "SELECT parent_id=%lld AND original_parent_id=%lld FROM entries"
                      " WHERE id=%lld", (long long)cmp, (long long)a0, (long long)u);
    assert(db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", sid, -1) == fin);
    expect_shape(db, sid, O, "user:hi|user:[Summary of earlier conversation]\nsum|user:go"
                             "|assistant:done");
    expect_shape(db, sid, A, "user:hi|user:[Summary of earlier conversation]\nsum*|user:go"
                             "|assistant:done");

    /* Compaction after an edit: the edited turn is summarized away like any
     * other. */
    int64_t u2 = add_user(db, sid, "more");
    assert(test_legacy_compact(db, sid, u0, u2, "sum2") > 0);
    expect_shape(db, sid, O, "user:hi|user:[Summary of earlier conversation]\nsum2|user:more");

    db_close(db); test_db_clean(DB_PATH);
}

/* ── The operations on any turn ────────────────────────────────────*/

static int64_t leaf_of(sqlite3 *db, int64_t sid) {
    return db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?", sid, -1);
}

/* Apply, and (want_leaf > 0) say where the leaf must be afterwards. */
static void apply_to(sqlite3 *db, int64_t sid, const char *reply, int64_t want_leaf) {
    char *err = NULL;
    TurnEditResult rc = turn_edit_apply(db, sid, reply, LABEL, &err);
    if (rc != TURN_EDIT_APPLIED) {
        fprintf(stderr, "FAIL apply %s: rc=%d %s\n", reply, (int)rc, err ? err : "");
        abort();
    }
    if (want_leaf > 0) assert(leaf_of(db, sid) == want_leaf);
    assert(sqlite3_get_autocommit(db));
}

static int64_t parent_of(sqlite3 *db, int64_t id) {
    return db_scalar_i64(db, "SELECT parent_id FROM entries WHERE id=?", id, -2);
}

/* Turn 3 of a seeded session, still running: two iterations, each a
 * reasoning entry, a message, one call and its result. */
typedef struct { int64_t u, it4, rs4, a4, k1, rk1, it5, rs5, a5, k2, rk2; } R;

static R add_running(sqlite3 *db, int64_t sid) {
    R r;
    r.u = add_user(db, sid, "more");
    r.it4 = db_next_iteration_id(db, sid);
    r.rs4 = add_reasoning(db, sid, r.it4, "why4");
    r.a4 = add_typed(db, sid, r.it4, "assistant_message", "four", NULL, NULL);
    r.k1 = add_call(db, sid, r.it4, "k1", "look", "{}", "done");
    r.rk1 = add_typed(db, sid, r.it4, "tool_result", "rk1", "k1", "look");
    r.it5 = db_next_iteration_id(db, sid);
    r.rs5 = add_reasoning(db, sid, r.it5, "why5");
    r.a5 = add_typed(db, sid, r.it5, "assistant_message", "five", NULL, NULL);
    r.k2 = add_call(db, sid, r.it5, "k2", "look", "{}", "done");
    r.rk2 = add_typed(db, sid, r.it5, "tool_result", "rk2", "k2", "look");
    assert(db_scalar_i64(db, "SELECT turn_id FROM entries WHERE id=?", r.rk2, -1) == 3);
    return r;
}

/* drop / replace / note / rewrite on a turn that is neither the newest nor
 * running, named by turn_id; check 3 holds there too, and a rewrite cannot
 * launder a call that stored a credential into a droppable one. */
static void test_past_turn(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "past", 0);
    int64_t f3 = add_plain_turn(db, t.sid, "next", "sure"), u3 = parent_of(db, f3);
    char r[512];
    snprintf(r, sizeof(r), E(
        "{\"op\":\"drop\",\"call_id\":\"c2\",\"turn_id\":2},"
        "{\"op\":\"replace\",\"call_id\":\"c1\",\"turn_id\":2,\"text\":\"R-ONE\"},"
        "{\"op\":\"note\",\"text\":\"past note\",\"before\":%lld}"), (long long)t.rs2);
    apply_to(db, t.sid, r, f3);
    int64_t n1 = leaf_rec_id(db, t.sid, ".replaced[0].replacement");
    int64_t note = leaf_rec_id(db, t.sid, ".notes[0]");
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.a1, t.c1, t.c3, n1, t.r3, note, t.rs2, t.a2, t.c4,
           t.r4, t.rs3, t.fin, u3, f3);
    /* The record is on the leaf — the newest turn's answer — and names the
     * first entry the edit changed. */
    expect_q(db, "1", "SELECT json_extract(data,'$.turn_edits[0].left_branch')"
                      " = json_array(%lld,%lld,%lld,%lld)"
                      " AND json_extract(data,'$.turn_edits[0].thinking_cut')=%lld"
                      " FROM entries WHERE id=%lld", (long long)t.rs1, (long long)t.c2,
             (long long)t.r2, (long long)t.r1, (long long)t.a1, (long long)f3);
    /* What was written says so (P4): who edited it, and the original it
     * stands for; its own source stays the original's. */
    expect_q(db, "1", "SELECT json_extract(data,'$.edited_by')='" LABEL "'"
                      " AND json_extract(data,'$.source') IS NULL"
                      " AND json_extract(data,'$.replaces')=%lld AND turn_id=2"
                      " AND " TURN_EDIT_SQL_STANDS_IN("data") " FROM entries WHERE id=%lld",
             (long long)t.r1, (long long)n1);
    expect_q(db, "2", "SELECT turn_id FROM entries WHERE id=%lld", (long long)note);
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c3|tool:R-ONE,<c1|tool:r3,<c3"
        "|user:" NOTE_TAG "past note|assistant:two,=c4|tool:r4,<c4|assistant:done"
        "|user:next|assistant:sure");

    /* Check 3 on a past turn: the effect call stays, but may be rewritten. */
    const TurnEditResult NO = TURN_EDIT_REJECTED;
    expect_untouched(db, t.sid, E("{\"op\":\"drop\",\"call_id\":\"c3\",\"turn_id\":2}"), NO,
                     "protected");
    apply_to(db, t.sid, E("{\"op\":\"rewrite\",\"call_id\":\"c3\",\"turn_id\":2,"
                          "\"args\":{\"k\":30}}"), f3);
    int64_t c3n = leaf_rec_id(db, t.sid, ".replaced[0].replacement");
    expect_q(db, "{\"k\":30}|c3|act|2", "SELECT concat_ws('|', content, tool_call_id,"
             " tool_name, turn_id) FROM entries WHERE id=%lld", (long long)c3n);
    expect_untouched(db, t.sid, E("{\"op\":\"drop\",\"call_id\":\"c3\",\"turn_id\":2}"), NO,
                     "protected");
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c3|user:<c1,<c3"
        "|user:" NOTE_TAG "past note|assistant:two,=c4|user:<c4|assistant:done*"
        "|user:next|assistant:sure");
    expect_body(db, t.sid, O, "$.messages[3].tool_calls[1].function.arguments", "{\"k\":30}");
    /* A past turn's answer is a message like any other. */
    snprintf(r, sizeof(r), E("{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"done, briefly\"}"),
             (long long)t.fin);
    apply_to(db, t.sid, r, f3);
    expect_text(db, t.sid, "done, briefly", 1);

    /* A retrieving tool that saved a secret is protected; rewriting its
     * arguments does not change what the call did. */
    int64_t sid = session_create(db, "secret", "default", -1, 0);
    add_user(db, sid, "store it");
    int64_t it = db_next_iteration_id(db, sid);
    add_call(db, sid, it, "s1", "fetch", "{\"url\":\"u\",\"save_secret\":\"K\"}", "done");
    add_typed(db, sid, it, "tool_result", "saved", "s1", "fetch");
    int64_t fin = add_typed(db, sid, db_next_iteration_id(db, sid), "assistant_message",
                            "ok", NULL, NULL);
    apply_to(db, sid, E("{\"op\":\"rewrite\",\"call_id\":\"s1\",\"args\":{\"url\":\"u\"}}"), fin);
    char *in = input_turn(db, sid);
    assert(in);
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[0].protected')", "true");
    expect_at(db, in, "$.iterations[0].calls[0].args.url", "u");
    free(in);
    expect_untouched(db, sid, E(DROP("s1")), NO, "protected");

    db_close(db); test_db_clean(DB_PATH);
}

/* drop_turns and summarise by turn: whole turns before the newest, written
 * as one summary entry; the checks on the range; check 2 across a turn. */
static void test_whole_turns(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "turns", 0);
    int64_t f3 = add_plain_turn(db, t.sid, "three", "3"), u3 = parent_of(db, f3);
    int64_t f4 = add_plain_turn(db, t.sid, "four", "4"), u4 = parent_of(db, f4);
    const TurnEditResult NO = TURN_EDIT_REJECTED;
    #define REJECT(reply, why) expect_untouched(db, t.sid, reply, NO, why)
    static const struct { const char *reply, *why; } cases[] = {
        { E("{\"op\":\"drop_turns\",\"from_turn\":3,\"to_turn\":4}"), "live turn" },
        { E("{\"op\":\"drop_turns\",\"from_turn\":3,\"to_turn\":2}"), "is after" },
        { E("{\"op\":\"drop_turns\",\"from_turn\":9,\"to_turn\":9}"), "not in this session" },
        { E("{\"op\":\"drop_turns\",\"from_turn\":1}"), "needs \"from_turn\"" },
        { E("{\"op\":\"drop_turns\",\"from_turn\":1,\"to_turn\":1,\"text\":\"x\"}"),
          "does not take" },
        { E("{\"op\":\"summarise\",\"from_turn\":1,\"to_turn\":1}"), "needs \"text\"" },
        { E("{\"op\":\"summarise\",\"text\":\"s\",\"from_turn\":1,\"to_iteration\":1}"),
          "needs \"from_turn\"" },
        { E("{\"op\":\"summarise\",\"text\":\"s\",\"from_turn\":\"1\",\"to_turn\":1}"),
          "must be an entry, turn or iteration id" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("    reject[%zu]\n", i);
        REJECT(cases[i].reply, cases[i].why);
    }

    /* Turns 1-2 become one summary at the root; the protected call left
     * with its whole turn (check 3). */
    apply_to(db, t.sid, E("{\"op\":\"summarise\",\"from_turn\":1,\"to_turn\":2,"
                          "\"text\":\"S12\"}"), f4);
    int64_t sum = leaf_rec_id(db, t.sid, ".summaries[0]");
    BRANCH(db, t.sid, sum, u3, f3, u4, f4);
    expect_q(db, "compaction|4|1|-1|" LABEL, "SELECT concat_ws('|', type, role, turn_id,"
             " parent_id, json_extract(data,'$.source')) FROM entries WHERE id=%lld"
             " AND iteration_id IS NULL", (long long)sum);
    expect_q(db, "17", "SELECT json_array_length(data,'$.turn_edits[0].left_branch')"
                       " FROM entries WHERE id=%lld", (long long)f4);
    expect_q(db, "1", "SELECT json_extract(data,'$.turn_edits[0].thinking_cut')=%lld"
                      " FROM entries WHERE id=%lld", (long long)sum, (long long)f4);
    expect_shape(db, t.sid, O, "user:[Summary of earlier conversation]\nS12|user:three"
                               "|assistant:3|user:four|assistant:4");
    expect_shape(db, t.sid, G, "user:[Summary of earlier conversation]\nS12|user:three"
                               "|model:3|user:four|model:4");
    expect_text(db, t.sid, "hello", 0);
    /* Read back as a summary unit standing before turn 3. */
    char *in = input_all(db, t.sid);
    assert(in);
    expect_json(db, in, "json_array_length(?1,'$.history')", "2");
    expect_at(db, in, "$.history[0].summary.text", "S12");
    expect_at(db, in, "$.history[1].turn_id", "3");
    expect_at(db, in, "$.history_omitted", "0");
    free(in);
    TurnEditInputSpec one = { 1, 1, TURN_EDIT_INPUT_MAX_BYTES };
    in = turn_edit_input(db, t.sid, &one, &STOP, NULL);
    expect_json(db, in, "json_array_length(?1,'$.history')", "1");
    expect_at(db, in, "$.history[0].turn_id", "3");
    free(in);

    /* drop_turns: turn 3 leaves; turn 4 hangs off the summary and remembers
     * its first parent. */
    apply_to(db, t.sid, E("{\"op\":\"drop_turns\",\"from_turn\":3,\"to_turn\":3}"), f4);
    BRANCH(db, t.sid, sum, u4, f4);
    expect_q(db, "1", "SELECT original_parent_id=%lld FROM entries WHERE id=%lld",
             (long long)f3, (long long)u4);
    #undef REJECT

    /* Check 2: a pending call keeps its whole turn; a background job does not. */
    int64_t sid = session_create(db, "pending", "default", -1, 0);
    add_user(db, sid, "go");
    int64_t it = db_next_iteration_id(db, sid);
    add_typed(db, sid, it, "assistant_message", "starting", NULL, NULL);
    add_call(db, sid, it, "p1", "look", "{}", "pending");
    int64_t fin = add_plain_turn(db, sid, "and", "then");
    expect_untouched(db, sid, E("{\"op\":\"drop_turns\",\"from_turn\":1,\"to_turn\":1}"), NO,
                     "pending");
    run(db, "UPDATE tool_calls SET status='background' WHERE call_id='p1'");
    apply_to(db, sid, E("{\"op\":\"drop_turns\",\"from_turn\":1,\"to_turn\":1}"), fin);

    db_close(db); test_db_clean(DB_PATH);
}

/* The last finished turn: rewrite, and summarise by iteration — never its
 * answer. Overlapping edits and a bad edit beside good ones: refused whole. */
static void test_finished_turn_ops(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "fin", 0);
    const TurnEditResult NO = TURN_EDIT_REJECTED;
    char r[512];
    #define REJECT(reply, why) expect_untouched(db, t.sid, reply, NO, why)
    snprintf(r, sizeof(r), E("{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"x\"}"),
             (long long)t.fin);
    REJECT(r, "never edited");
    snprintf(r, sizeof(r), E("{\"op\":\"summarise\",\"text\":\"s\",\"from_iteration\":%lld,"
             "\"to_iteration\":%lld}"), (long long)t.it1, (long long)t.it3);
    REJECT(r, "answer");
    snprintf(r, sizeof(r), E("{\"op\":\"summarise\",\"text\":\"s\",\"from_iteration\":%lld,"
             "\"to_iteration\":%lld}"), (long long)t.it2, (long long)t.it1);
    REJECT(r, "is after to_iteration");
    snprintf(r, sizeof(r), E("{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"x\"}"),
             (long long)t.r1);
    REJECT(r, "not a user or assistant message");
    static const struct { const char *reply, *why; } cases[] = {
        { E("{\"op\":\"summarise\",\"text\":\"s\",\"from_iteration\":1,\"to_iteration\":1}"),
          "not an iteration of the newest turn" },
        { E("{\"op\":\"rewrite\",\"entry_id\":99999,\"text\":\"x\"}"), "not on the session's branch" },
        { E("{\"op\":\"rewrite\",\"call_id\":\"c1\",\"text\":\"x\"}"), "takes \"args\"" },
        { E("{\"op\":\"rewrite\",\"call_id\":\"c1\",\"args\":[1]}"), "takes \"args\"" },
        { E("{\"op\":\"rewrite\",\"entry_id\":3,\"call_id\":\"c1\",\"text\":\"x\"}"),
          "one of them" },
        { E("{\"op\":\"rewrite\",\"entry_id\":3,\"text\":\"x\",\"turn_id\":2}"),
          "takes \"text\"" },
        { E("{\"op\":\"rewrite\",\"call_id\":\"c1\","
            "\"args\":{\"k\":\"ghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzQ\"}}"),
          "contains a secret" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("    reject[%zu]\n", i);
        REJECT(cases[i].reply, cases[i].why);
    }
    /* Overlaps: each entry is named by one edit at most. */
    snprintf(r, sizeof(r), E("{\"op\":\"summarise\",\"text\":\"s\",\"from_iteration\":%lld,"
             "\"to_iteration\":%lld}," DROP("c1")), (long long)t.it1, (long long)t.it1);
    REJECT(r, "named by two edits");
    snprintf(r, sizeof(r), E("{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"x\"},"
             "{\"op\":\"drop_turns\",\"from_turn\":1,\"to_turn\":1}"), (long long)t.a0);
    REJECT(r, "is already named by edits[0]");
    snprintf(r, sizeof(r), E("{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"x\"}," DROP("c4")),
             (long long)t.a2);
    REJECT(r, "a drop empties");
    /* Atomicity: good edits beside one bad one leave nothing behind. */
    snprintf(r, sizeof(r), E("{\"op\":\"note\",\"text\":\"fine\"},"
             "{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"fine\"},"
             "{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"fine\"},"
             "{\"op\":\"rewrite\",\"entry_id\":99999,\"text\":\"bad\"}"), (long long)t.u);
    REJECT(r, "not on the session's branch");
    #undef REJECT

    /* A user message and an iteration's text, rewritten; the iteration whose
     * text changed loses its reasoning (its blob is the response as written). */
    snprintf(r, sizeof(r), E("{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"GO!\"},"
             "{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"ONE!\"}"),
             (long long)t.u, (long long)t.a1);
    apply_ok(db, &t, r);
    int64_t un = leaf_rec_id(db, t.sid, ".replaced[0].replacement");
    int64_t an = leaf_rec_id(db, t.sid, ".replaced[1].replacement");
    BRANCH(db, t.sid, t.u0, t.a0, un, an, t.c1, t.c2, t.c3, t.r2, t.r1, t.r3, t.rs2, t.a2,
           t.c4, t.r4, t.rs3, t.fin);
    expect_q(db, "user_message|1|2|GO!", "SELECT concat_ws('|', type, role, turn_id, content)"
             " FROM entries WHERE id=%lld", (long long)un);
    expect_q(db, "assistant_message|2|2", "SELECT concat_ws('|', type, role, iteration_id - %lld"
             " + 2) FROM entries WHERE id=%lld", (long long)t.it1, (long long)an);
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:GO!|assistant:ONE!,=c1,=c2,=c3|tool:r2,<c2|tool:r1,<c1"
        "|tool:r3,<c3|assistant:two,=c4|tool:r4,<c4|assistant:done");
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details", NULL);
    expect_body(db, t.sid, O, "$.messages[7].reasoning_details[0].text", "why2");
    /* A rewritten message is not delivered again: it is the original's. */
    expect_q(db, "0", "SELECT COUNT(*) FROM entries WHERE id=%lld AND "
             "CASE WHEN json_valid(data) THEN json_extract(data,'$.replaces') END IS NULL",
             (long long)an);

    /* Summarise the two working iterations of the finished turn. */
    snprintf(r, sizeof(r), E("{\"op\":\"summarise\",\"text\":\"worked\",\"from_iteration\":%lld,"
             "\"to_iteration\":%lld}"), (long long)t.it1, (long long)t.it2);
    apply_ok(db, &t, r);
    int64_t sum = leaf_rec_id(db, t.sid, ".summaries[0]");
    BRANCH(db, t.sid, t.u0, t.a0, un, sum, t.rs3, t.fin);
    expect_q(db, "2", "SELECT turn_id FROM entries WHERE id=%lld", (long long)sum);
    expect_shape(db, t.sid, O, "user:hi|assistant:hello|user:GO!"
                               "|user:[Summary of earlier conversation]\nworked|assistant:done");
    /* In the input it stands with the turn's notes, marked. */
    char *in = input_turn(db, t.sid);
    assert(in);
    expect_json(db, in, "json_array_length(?1,'$.iterations')", "0");
    expect_id_at(db, in, "$.notes[0].entry_id", sum);
    expect_json(db, in, "json_type(?1,'$.notes[0].summary')", "true");
    expect_id_at(db, in, "$.notes[0].before_iteration", t.it3);
    free(in);
    /* ...and as an earlier turn, still one turn. */
    int64_t f3 = add_plain_turn(db, t.sid, "next", "sure");
    in = input_all(db, t.sid);
    expect_json(db, in, "json_array_length(?1,'$.history')", "2");
    expect_at(db, in, "$.history[1].turn_id", "2");
    expect_json(db, in, "json_type(?1,'$.history[1].notes[0].summary')", "true");
    expect_id_at(db, in, "$.history[1].final.entry_id", t.fin);
    free(in);
    (void)f3;

    db_close(db); test_db_clean(DB_PATH);
}

/* The running turn, between iterations: a ContextThreshold reply edits it,
 * the leaf may move, a note lands at the end, and the record says where the
 * edit began. */
static void test_running_turn(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "running", 0);
    R w = add_running(db, t.sid);
    run(db, "UPDATE entries SET tokens=777 WHERE id=%lld", (long long)w.rk1);
    TurnEditInputSpec spec = { 1, 1, TURN_EDIT_INPUT_DEFAULT_BYTES };
    /* A stop event needs the answer: nothing is built mid-turn. */
    assert(turn_edit_input(db, t.sid, &spec, &STOP, NULL) == NULL);
    /* A ContextThreshold script gets the turn as it stands (no answer yet),
     * the numbers it decides on as "context", and every entry's size. */
    TurnEditInputFor ct = { .event = "ContextThreshold", .need_answer = 0,
        .context = "{\"tokens\":9000,\"window\":10000}" };
    char *in = turn_edit_input(db, t.sid, &spec, &ct, NULL);
    assert(in);
    expect_at(db, in, "$.event", "ContextThreshold");
    expect_at(db, in, "$.context", "{\"tokens\":9000,\"window\":10000}");
    expect_json(db, in, "json_type(?1,'$.final')", "null");
    expect_json(db, in, "json_array_length(?1,'$.iterations')", "2");
    expect_id_at(db, in, "$.iterations[0].iteration_id", w.it4);
    expect_id_at(db, in, "$.iterations[0].entry_id", w.rs4);
    expect_id_at(db, in, "$.iterations[0].message_id", w.a4);
    expect_id_at(db, in, "$.iterations[0].calls[0].tokens",
                 db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE id=?", w.k1, -1));
    /* A measured count wins over the estimate. */
    expect_at(db, in, "$.iterations[0].calls[0].result.tokens", "777");
    expect_at(db, in, "$.history[0].final.text", "done");
    free(in);

    /* The oldest iteration summarised, the newest result shortened: the
     * stand-in is the new leaf. */
    char r[512];
    snprintf(r, sizeof(r), E("{\"op\":\"summarise\",\"text\":\"step four\",\"from_iteration\":%lld,"
             "\"to_iteration\":%lld},{\"op\":\"replace\",\"call_id\":\"k2\",\"text\":\"short\"}"),
             (long long)w.it4, (long long)w.it4);
    apply_to(db, t.sid, r, 0);
    int64_t sum = leaf_rec_id(db, t.sid, ".summaries[0]");
    int64_t rk2n = leaf_rec_id(db, t.sid, ".replaced[0].replacement");
    assert(leaf_of(db, t.sid) == rk2n && rk2n > sum && sum > w.rk2);
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, t.r1, t.r3, t.rs2,
           t.a2, t.c4, t.r4, t.rs3, t.fin, w.u, sum, w.rs5, w.a5, w.k2, rk2n);
    expect_q(db, "3", "SELECT turn_id FROM entries WHERE id=%lld AND iteration_id IS NULL",
             (long long)sum);
    assert(leaf_rec_id(db, t.sid, ".thinking_cut") == sum);
    expect_shape(db, t.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c1,=c2,=c3|tool:r2,<c2|tool:r1,<c1"
        "|tool:r3,<c3|assistant:two,=c4|tool:r4,<c4|assistant:done|user:more"
        "|user:[Summary of earlier conversation]\nstep four|assistant:five,=k2|tool:short,<k2");
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c2,=c3|user:<c2,<c1,<c3"
        "|assistant:two,=c4|user:<c4|assistant:done*|user:more"
        "|user:[Summary of earlier conversation]\nstep four|assistant:five,=k2|user:<k2");
    in = turn_edit_input(db, t.sid, &spec, &ct, NULL);
    expect_json(db, in, "json_array_length(?1,'$.iterations')", "1");
    expect_json(db, in, "json_type(?1,'$.notes[0].summary')", "true");
    expect_id_at(db, in, "$.notes[0].before_iteration", w.it5);
    free(in);

    /* A note with no "before", while the turn runs, goes at the end. */
    apply_to(db, t.sid, E("{\"op\":\"note\",\"text\":\"keep going\"}"), 0);
    int64_t note = leaf_of(db, t.sid);
    assert(parent_of(db, note) == rk2n);
    expect_q(db, "1", "SELECT json_extract(data,'$.turn_edits[0].notes[0]')=id AND turn_id=3"
                      " FROM entries WHERE id=%lld", (long long)note);
    /* Dropping the newest call empties its iteration: it leaves whole, the
     * leaf stays the note. */
    apply_to(db, t.sid, E(DROP("k2")), note);
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, t.r1, t.r3, t.rs2,
           t.a2, t.c4, t.r4, t.rs3, t.fin, w.u, sum, note);

    db_close(db); test_db_clean(DB_PATH);
}

/* A long turn summarised again: the summary of its earlier iterations, and
 * a note, stand right before the next run summarised — they leave with it,
 * so the turn carries one summary, not a stack. A note further back stays. */
static void test_running_turn_refold(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "refold", 0);
    R w = add_running(db, t.sid);
    char r[512];
    snprintf(r, sizeof(r), E("{\"op\":\"summarise\",\"text\":\"step four\",\"from_iteration\":%lld,"
             "\"to_iteration\":%lld},{\"op\":\"note\",\"text\":\"heads up\",\"before\":%lld}"),
             (long long)w.it4, (long long)w.it4, (long long)w.rs5);
    apply_to(db, t.sid, r, w.rk2);
    int64_t sum1 = leaf_rec_id(db, t.sid, ".summaries[0]");
    int64_t note = leaf_rec_id(db, t.sid, ".notes[0]");
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, t.r1, t.r3, t.rs2,
           t.a2, t.c4, t.r4, t.rs3, t.fin, w.u, sum1, note, w.rs5, w.a5, w.k2, w.rk2);
    int64_t it6 = db_next_iteration_id(db, t.sid);
    int64_t a6 = add_typed(db, t.sid, it6, "assistant_message", "six", NULL, NULL);
    int64_t k3 = add_call(db, t.sid, it6, "k3", "look", "{}", "done");
    int64_t rk3 = add_typed(db, t.sid, it6, "tool_result", "rk3", "k3", "look");
    snprintf(r, sizeof(r), E("{\"op\":\"summarise\",\"text\":\"steps four and five\","
             "\"from_iteration\":%lld,\"to_iteration\":%lld}"), (long long)w.it5, (long long)w.it5);
    apply_to(db, t.sid, r, rk3);
    int64_t sum2 = leaf_rec_id(db, t.sid, ".summaries[0]");
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, t.r1, t.r3, t.rs2,
           t.a2, t.c4, t.r4, t.rs3, t.fin, w.u, sum2, a6, k3, rk3);
    expect_q(db, "1", "SELECT json_extract(data,'$.turn_edits[0].left_branch') ="
             " json_array(%lld,%lld,%lld,%lld,%lld,%lld) FROM entries WHERE id=%lld",
             (long long)sum1, (long long)note, (long long)w.rs5, (long long)w.a5,
             (long long)w.k2, (long long)w.rk2, (long long)rk3);
    db_close(db); test_db_clean(DB_PATH);
}

/* Accounting of what an edit writes: a stand-in for a measured entry keeps
 * its count scaled by length, a short or unmeasured one and a new entry take
 * the estimate; and a report older than an edit no longer measures the head. */
static void test_written_tokens(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "acct", 0);
    run(db, "UPDATE entries SET content=printf('%%.600c','x'), content_bytes=600, tokens=150"
            " WHERE id=%lld", (long long)t.r1);
    run(db, "UPDATE entries SET tokens=10 WHERE id=%lld", (long long)t.r4);
    run(db, "UPDATE entries SET content=printf('%%.400c','u'), content_bytes=400, tokens=100"
            " WHERE id=%lld", (long long)t.u);
    run(db, "UPDATE entries SET usage_in=100000 WHERE id=%lld", (long long)t.fin);
    ContextPlan m = {0};
    assert(context_measure(db, t.sid, 77, &m) == 0);
    assert(m.head > 77);                    /* measured from the answer's report */
    context_plan_free(&m);

    char text300[301], text200[201], r[1024];
    memset(text300, 'y', 300); text300[300] = 0;
    memset(text200, 'v', 200); text200[200] = 0;
    snprintf(r, sizeof(r), E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"%s\"},"
             "{\"op\":\"replace\",\"call_id\":\"c4\",\"text\":\"tiny\"},"
             "{\"op\":\"rewrite\",\"entry_id\":%lld,\"text\":\"%s\"},"
             "{\"op\":\"note\",\"text\":\"a note\"}"), text300, (long long)t.u, text200);
    apply_ok(db, &t, r);
    expect_q(db, "75|-|50", "SELECT group_concat(COALESCE(n.tokens,'-'),'|') FROM"
             " (SELECT json_extract(value,'$.replacement') AS id, key FROM json_each("
             "   (SELECT data FROM entries WHERE id=%lld),'$.turn_edits[0].replaced')) x"
             " JOIN entries n ON n.id=x.id ORDER BY x.key", (long long)t.fin);
    expect_q(db, "1", "SELECT tokens IS NULL AND token_estimate=%d FROM entries"
                      " WHERE id=(SELECT json_extract(data,'$.turn_edits[0].notes[0]')"
                      " FROM entries WHERE id=%lld)", TOKEN_ESTIMATE(6), (long long)t.fin);
    /* The answer now carries the record: its report predates the edit. */
    assert(context_measure(db, t.sid, 77, &m) == 0);
    assert(m.head == 77);
    context_plan_free(&m);

    db_close(db); test_db_clean(DB_PATH);
}

int main(void) {
    TEST_INIT();
    printf("test_turn_edit:\n");
    RUN(test_input);
    RUN(test_note_after_answer);
    RUN(test_rejections);
    RUN(test_drop_one_of_batch);
    RUN(test_replace);
    RUN(test_notes);
    RUN(test_record);
    RUN(test_history);
    RUN(test_input_options);
    RUN(test_input_reasoning);
    RUN(test_turn_edges);
    RUN(test_past_turn);
    RUN(test_whole_turns);
    RUN(test_finished_turn_ops);
    RUN(test_running_turn);
    RUN(test_running_turn_refold);
    RUN(test_written_tokens);
    printf("All turn edit tests passed.\n");
    return 0;
}
