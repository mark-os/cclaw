/* turn_edit.c: the finished turn as a hook's input, and the edits a
 * turnSettled script sends back — checked, then applied all or nothing — on a
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

/* The input as a hook that declares nothing gets it: the finished turn. */
static char *input_turn(sqlite3 *db, int64_t sid) {
    TurnEditInputSpec spec = TURN_EDIT_INPUT_DEFAULTS;
    return turn_edit_input(db, sid, &spec, NULL);
}

/* ...and as one that asked for everything, with room for it. */
static char *input_all(sqlite3 *db, int64_t sid) {
    TurnEditInputSpec spec = { TURN_EDIT_HISTORY_ALL, 1, TURN_EDIT_INPUT_MAX_BYTES };
    return turn_edit_input(db, sid, &spec, NULL);
}

static char *input_as(sqlite3 *db, int64_t sid, int history, int results, int max_bytes,
                      int *too_big) {
    TurnEditInputSpec spec = { history, results, max_bytes };
    return turn_edit_input(db, sid, &spec, too_big);
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

/* Expected branch from a list of ids ended by 0. */
static void expect_branch_ids(sqlite3 *db, int64_t sid, ...) {
    char want[512] = "";
    va_list ap;
    va_start(ap, sid);
    for (int64_t id; (id = va_arg(ap, int64_t)) != 0;) {
        size_t n = strlen(want);
        snprintf(want + n, sizeof(want) - n, "%s%lld", n ? "," : "", (long long)id);
    }
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
    char want[256] = "[";
    va_list ap;
    va_start(ap, path);
    for (int64_t id; (id = va_arg(ap, int64_t)) != 0;) {
        size_t n = strlen(want);
        snprintf(want + n, sizeof(want) - n, "%s%lld", n > 1 ? "," : "", (long long)id);
    }
    va_end(ap);
    strcat(want, "]");
    expect_rec(db, t, path, want);
}
#define REC_IDS(db, t, path, ...) expect_rec_ids(db, t, path, __VA_ARGS__, (int64_t)0)

static int64_t turn_tokens(sqlite3 *db, const T *t) {
    char *v = q(db,
        "WITH RECURSIVE b(id, lvl) AS (SELECT %lld, 0 UNION ALL"
        "  SELECT e.parent_id, b.lvl+1 FROM entries e JOIN b ON e.id=b.id WHERE b.lvl<1000)"
        " SELECT SUM(e.token_estimate) FROM b JOIN entries e ON e.id=b.id WHERE e.turn_id=2",
        (long long)t->fin);
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
    IN("$.event", "turnSettled");
    /* What it was given, so a script can tell "no history asked for" from "a
     * first turn": nothing declared is the turn alone. The turn before it
     * (u0, a0) is not sent and not counted as left out. */
    char want_echo[128];
    snprintf(want_echo, sizeof(want_echo),
             "{\"history\":\"none\",\"history_results\":true,\"max_bytes\":%d}",
             TURN_EDIT_INPUT_DEFAULT_BYTES);
    IN("$.input", want_echo);
    expect_json(db, in, "json_type(?1,'$.history')", "array");
    expect_json(db, in, "json_array_length(?1,'$.history')", "0");
    IN("$.history_omitted", "0");
    IN("$.agent", "default");
    expect_id_at(db, in, "$.session_id", t.sid);
    IN("$.turn_id", "2");
    expect_json(db, in, "json_array_length(?1,'$.user_messages')", "1");
    expect_id_at(db, in, "$.user_messages[0].entry_id", t.u);
    IN("$.user_messages[0].text", "go");
    /* The system entry already in the turn, and the iteration it precedes. */
    expect_json(db, in, "json_array_length(?1,'$.notes')", "1");
    expect_id_at(db, in, "$.notes[0].entry_id", t.sys);
    IN("$.notes[0].text", "sys mid");
    expect_id_at(db, in, "$.notes[0].before_iteration", t.it2);

    expect_json(db, in, "json_array_length(?1,'$.iterations')", "2");
    expect_id_at(db, in, "$.iterations[0].iteration_id", t.it1);
    /* An iteration's entry_id is its first row: here the reasoning row. */
    expect_id_at(db, in, "$.iterations[0].entry_id", t.rs1);
    IN("$.iterations[0].text", "one");
    /* A parallel batch: calls in call order, whatever order the results
     * came back in. */
    expect_json(db, in, "json_array_length(?1,'$.iterations[0].calls')", "3");
    IN("$.iterations[0].calls[0].call_id", "c1");
    expect_id_at(db, in, "$.iterations[0].calls[0].entry_id", t.c1);
    IN("$.iterations[0].calls[0].tool", "look");
    IN("$.iterations[0].calls[0].args.path", "/a");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[0].protected')", "false");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[0].is_error')", "false");
    expect_id_at(db, in, "$.iterations[0].calls[0].result.entry_id", t.r1);
    expect_id_at(db, in, "$.iterations[0].calls[0].result.tokens",
                 db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE id=?", t.r1, -1));
    /* Verbatim: no line numbers, whitespace kept. */
    IN("$.iterations[0].calls[0].result.text", "line one\n  line two");
    /* Arguments that are not JSON come through as the stored string. */
    IN("$.iterations[0].calls[1].call_id", "c2");
    IN("$.iterations[0].calls[1].args", "oops");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[1].args')", "text");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[1].protected')", "false");
    IN("$.iterations[0].calls[1].result.text", "r2");
    /* An effect tool is protected; its failed result is flagged. */
    IN("$.iterations[0].calls[2].call_id", "c3");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[2].protected')", "true");
    expect_json(db, in, "json_type(?1,'$.iterations[0].calls[2].is_error')", "true");
    expect_id_at(db, in, "$.iterations[1].iteration_id", t.it2);
    expect_id_at(db, in, "$.iterations[1].entry_id", t.rs2);
    IN("$.iterations[1].text", "two");
    IN("$.iterations[1].calls[0].result.text", "r4");
    /* The answer is reported apart and is no iteration. */
    IN("$.final.text", "done");
    expect_id_at(db, in, "$.final.entry_id", t.fin);
    expect_id_at(db, in, "$.final.iteration_id", t.it3);

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
    in = input_turn(db, p.sid);
    assert(in);
    expect_json(db, in,
        "(SELECT json_group_array(json_extract(c.value,'$.protected'))"
        "  FROM json_each(?1,'$.iterations') i, json_each(i.value,'$.calls') c)",
        "[1,1,1,1]");
    free(in);
    /* Without save_secret the same fetch is droppable; a tool with no tools
     * row is protected. */
    run(db, "UPDATE entries SET content='{\"url\":\"u\"}' WHERE id=%lld", (long long)p.c1);
    run(db, "UPDATE entries SET tool_name='unregistered' WHERE id=%lld", (long long)p.c3);
    in = input_turn(db, p.sid);
    assert(in);
    expect_json(db, in,
        "(SELECT json_group_array(json_extract(c.value,'$.protected'))"
        "  FROM json_each(?1,'$.iterations') i, json_each(i.value,'$.calls') c)",
        "[0,1,1,1]");
    free(in);

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
    printf("  PASS test_input\n");
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
    /* Shape. */
    REJECT("nope", "not a JSON object");
    REJECT("[]", "not a JSON object");
    REJECT("\"drop\"", "not a JSON object");
    REJECT("", "not a JSON object");
    REJECT("{\"edits\":[],\"summary\":\"x\"}", "unknown key \"summary\"");
    REJECT("{\"drop\":[\"c1\"]}", "unknown key \"drop\"");
    REJECT("{\"edits\":[],\"edits\":[" DROP("c2") "]}", "appears twice");
    REJECT("{\"edits\":{}}", "\"edits\" must be an array");
    REJECT("{\"edits\":null}", "\"edits\" must be an array");
    /* A bad record is refused even with no edits: shape comes first. */
    REJECT("{\"record\":[]}", "\"record\" must be an object");
    REJECT("{\"edits\":[],\"record\":\"x\"}", "\"record\" must be an object");
    REJECT("{\"edits\":[" DROP("c2") "],\"record\":5}", "\"record\" must be an object");
    {
        size_t n = TURN_EDIT_RECORD_MAX_BYTES + 64;
        char *big = malloc(n);
        assert(big);
        strcpy(big, "{\"record\":{\"pad\":\"");
        size_t len = strlen(big);
        memset(big + len, 'x', TURN_EDIT_RECORD_MAX_BYTES);
        strcpy(big + len + TURN_EDIT_RECORD_MAX_BYTES, "\"}}");
        REJECT(big, "\"record\" is larger than 16384 bytes");
        free(big);
    }
    REJECT(E("5"), "edits[0] must be an object");
    REJECT(E(DROP("c2") ",7"), "edits[1] must be an object");
    REJECT(E("{\"op\":\"cut\",\"call_id\":\"c1\"}"), "edits[0] \"op\" must be");
    REJECT(E("{\"call_id\":\"c1\"}"), "edits[0] \"op\" must be");
    REJECT(E("{\"op\":1,\"call_id\":\"c1\"}"), "edits[0] \"op\" must be");
    REJECT(E("{\"op\":\"drop\",\"call_id\":\"c1\",\"call_id\":\"c2\"}"), "names a field twice");
    REJECT(E("{\"op\":\"drop\",\"call_id\":\"c1\",\"text\":\"x\"}"),
           "has a field its op does not take");
    REJECT(E("{\"op\":\"drop\",\"call_id\":\"c1\",\"why\":\"x\"}"),
           "has a field its op does not take");
    REJECT(E("{\"op\":\"note\",\"text\":\"x\",\"call_id\":\"c1\"}"),
           "has a field its op does not take");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"x\",\"before\":1}"),
           "has a field its op does not take");
    REJECT(E("{\"op\":\"drop\"}"), "edits[0] needs \"call_id\", a string");
    REJECT(E("{\"op\":\"drop\",\"call_id\":4}"), "needs \"call_id\", a string");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"c1\"}"), "needs \"text\", a non-empty string");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"\"}"), "needs \"text\"");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":5}"), "needs \"text\"");
    REJECT(E("{\"op\":\"note\"}"), "needs \"text\"");
    REJECT(E("{\"op\":\"note\",\"text\":[\"a\"]}"), "needs \"text\"");
    REJECT(E("{\"op\":\"note\",\"text\":\"x\",\"before\":\"5\"}"),
           "\"before\" must be an entry id");
    REJECT(E("{\"op\":\"note\",\"text\":\"x\",\"before\":1.5}"),
           "\"before\" must be an entry id");

    /* Call ids. */
    REJECT(E(DROP("zz")), "drop: \"zz\" is not a tool call of this turn");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"zz\",\"text\":\"x\"}"),
           "replace: \"zz\" is not a tool call");
    REJECT(E(DROP("c1") "," DROP("c1")), "named by two edits");
    REJECT(E(DROP("c1") ",{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"x\"}"),
           "named by two edits");
    /* A valid drop beside a bad one is refused with it. */
    REJECT(E(DROP("c2") "," DROP("c3")), "drop: call \"c3\" is protected");
    REJECT(E(DROP("c5")), "\"c5\" is not finished (background)");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"c5\",\"text\":\"x\"}"), "not finished");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"c6\",\"text\":\"x\"}"),
           "replace: call \"c6\" has no result to replace");

    /* Text: invisible-only is empty; a credential refuses the reply. */
    REJECT(E("{\"op\":\"note\",\"text\":\"\xE2\x80\x8B\"}"),
           "edits[0]: text is empty once invisible characters are removed");
    REJECT(E("{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"\xE2\x80\x8B\xE2\x80\x8C\"}"),
           "text is empty");
    REJECT(E("{\"op\":\"note\",\"text\":\"token was ghp_aB3kL9mXp7qR2wNv4jH8sT5uY1cF6gW0eDzQ\"}"),
           "edits[0]: text contains a secret");
    REJECT(E(DROP("c2") ",{\"op\":\"replace\",\"call_id\":\"c1\",\"text\":\"key AKIAIOSFODNN7EXAMPLE\"}"),
           "edits[1]: text contains a secret");

    /* Where a note may stand: only the first row of an iteration of this
     * turn, and not one this reply empties. */
    char r[256];
    #define NOTE_BEFORE(id) (snprintf(r, sizeof(r), \
        E("{\"op\":\"note\",\"text\":\"n\",\"before\":%lld}"), (long long)(id)), r)
    REJECT(NOTE_BEFORE(t.a1), "note: \"before\"");               /* rs1 opens it1 */
    REJECT(NOTE_BEFORE(t.c1), "is not the first entry of an iteration");
    REJECT(NOTE_BEFORE(t.r1), "is not the first entry of an iteration");
    REJECT(NOTE_BEFORE(t.u), "is not the first entry of an iteration");
    REJECT(NOTE_BEFORE(t.a0), "is not the first entry of an iteration");  /* earlier turn */
    REJECT(NOTE_BEFORE(t.fin), "is not the first entry of an iteration");
    REJECT(NOTE_BEFORE(99999), "is not the first entry of an iteration");
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

    /* The turn itself: not finished. */
    run(db, "UPDATE tool_calls SET status='running' WHERE call_id='c4'");
    REJECT(E(DROP("c2")), "\"c4\" is still running");
    run(db, "UPDATE tool_calls SET status='done' WHERE call_id='c4'");
    run(db, "UPDATE sessions SET leaf_id=%lld WHERE id=%lld", (long long)t.r4,
        (long long)t.sid);
    REJECT(E(DROP("c2")), "not an assistant message");
    REJECT(E("{\"op\":\"note\",\"text\":\"n\"}"), "not an assistant message");
    run(db, "UPDATE sessions SET leaf_id=%lld WHERE id=%lld", (long long)t.fin,
        (long long)t.sid);
    /* The next turn has opened: its calls are not this reply's to name. */
    int64_t s2 = session_create(db, "moved-on", "default", -1, 0);
    add_user(db, s2, "one");
    add_typed(db, s2, db_next_iteration_id(db, s2), "assistant_message", "a", NULL, NULL);
    add_user(db, s2, "two");
    expect_untouched(db, s2, E("{\"op\":\"note\",\"text\":\"n\"}"), NO,
                     "not an assistant message");
    expect_untouched(db, 99999, E("{\"op\":\"note\",\"text\":\"n\"}"), NO,
                     "not an assistant message");

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
    expect_untouched(db, s3, E(DROP("k2")), NO, "names more than one call in this turn");
    expect_untouched(db, s3, E("{\"op\":\"replace\",\"call_id\":\"k1\",\"text\":\"t\"}"), NO,
                     "replace: call \"k1\" has more than one result");

    /* A note before an iteration the same reply empties. */
    T d = seed_turn(db, "emptied", 0);
    char r2[256];
    snprintf(r2, sizeof(r2), E(DROP("c4") ",{\"op\":\"note\",\"text\":\"n\",\"before\":%lld}"),
             (long long)d.rs2);
    expect_untouched(db, d.sid, r2, NO, "names an iteration this reply drops");
    /* An answer whose data is not JSON: our side, refused, nothing written. */
    run(db, "UPDATE entries SET data='not json {' WHERE id=%lld", (long long)d.fin);
    expect_untouched(db, d.sid, E(DROP("c4")), TURN_EDIT_ERROR, "write failed");
    #undef REJECT
    #undef NOTE_BEFORE

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_rejections\n");
}

static void test_drop_one_of_batch(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "drop1", 0);
    int64_t before = turn_tokens(db, &t);
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
             (long long)before, (long long)turn_tokens(db, &t), (long long)t.fin);
    assert(turn_tokens(db, &t) < before);

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
    apply_ok(db, &t, E(DROP("c4")));

    /* Its only call gone, the iteration leaves whole: text and reasoning too. */
    BRANCH(db, t.sid, t.u0, t.a0, t.u, t.rs1, t.a1, t.c1, t.c2, t.c3, t.r2, t.r1, t.r3,
           t.rs3, t.fin);
    REC_IDS(db, &t, ".left_branch", t.rs2, t.a2, t.c4, t.r4);

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
    expect_text(db, t.sid, "why2", 0);
    /* The batch nobody touched still replays its reasoning. */
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details[0].text", "why1");

    /* Both droppable calls of a batch gone, the protected one staying, and
     * the other iteration emptied — in one reply. */
    T b = seed_turn(db, "dropboth", 0);
    apply_ok(db, &b, E(DROP("c4") "," DROP("c2") "," DROP("c1")));
    expect_shape(db, b.sid, O,
        "user:hi|assistant:hello|user:go|assistant:one,=c3|tool:r3,<c3|assistant:done");
    expect_shape(db, b.sid, G,
        "user:hi|model:hello|user:go|model:one,=act|user:<act|model:done");
    expect_shape(db, b.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c3|user:<c3|assistant:done");
    expect_body(db, b.sid, O, "$.messages[3].reasoning_details", NULL);
    REC_IDS(db, &b, ".left_branch", b.rs1, b.c1, b.c2, b.r2, b.r1, b.rs2, b.a2, b.c4, b.r4);
    free(branch(db, b.sid));

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_drop_whole_iteration\n");
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
    int64_t n1 = db_scalar_i64(db, "SELECT json_extract(data,'$.turn_edits[0].replaced[0].replacement')"
                                   " FROM entries WHERE id=?", t.fin, -1);
    int64_t n3 = db_scalar_i64(db, "SELECT json_extract(data,'$.turn_edits[0].replaced[1].replacement')"
                                   " FROM entries WHERE id=?", t.fin, -1);
    int64_t n4 = db_scalar_i64(db, "SELECT json_extract(data,'$.turn_edits[0].replaced[2].replacement')"
                                   " FROM entries WHERE id=?", t.fin, -1);
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
    printf("  PASS test_replace\n");
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
    int64_t n1 = db_scalar_i64(db, "SELECT json_extract(data,'$.turn_edits[0].notes[0]')"
                                   " FROM entries WHERE id=?", t.fin, -1);
    int64_t n2 = db_scalar_i64(db, "SELECT json_extract(data,'$.turn_edits[0].notes[1]')"
                                   " FROM entries WHERE id=?", t.fin, -1);
    int64_t n3 = db_scalar_i64(db, "SELECT json_extract(data,'$.turn_edits[0].notes[2]')"
                                   " FROM entries WHERE id=?", t.fin, -1);
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
    expect_shape(db, t.sid, G,
        "user:hi|model:hello|user:go|model:one,=look,=look2,=act|user:<look2|user:<look"
        "|user:<act|user:[system] sys mid|user:" NOTE_TAG "before two"
        "|model:two,=look|user:<look|user:" NOTE_TAG "first|user:" NOTE_TAG "second"
        "|model:done");
    expect_shape(db, t.sid, A,
        "user:hi|assistant:hello*|user:go|assistant:one,=c1,=c2,=c3"
        "|user:<c2,<c1,<c3|user:[system] sys mid|user:" NOTE_TAG "before two"
        "|assistant:two,=c4|user:<c4|user:" NOTE_TAG "first|user:" NOTE_TAG "second"
        "|assistant:done");
    expect_text(db, t.sid, "[system] " NOTE_TAG, 0);
    expect_text(db, t.sid, "[system] first", 0);
    expect_body(db, t.sid, O, "$.messages[3].reasoning_details[0].text", "why1");
    expect_body(db, t.sid, O, "$.messages[9].reasoning_details[0].text", "why2");
    expect_body(db, t.sid, O, "$.messages[13].reasoning_details[0].text", "why3");

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
    printf("  PASS test_notes\n");
}

/* The record across two applies, and the tree under it. */
static void test_record(void) {
    sqlite3 *db = open_seeded();
    T t = seed_turn(db, "record", 0);
    run(db, "UPDATE entries SET data='{\"keep\":\"me\",\"n\":[1,2]}' WHERE id=%lld",
        (long long)t.fin);
    int64_t before1 = turn_tokens(db, &t);

    apply_ok(db, &t, "{\"edits\":[" DROP("c4") "],"
                     "\"record\":{\"score\":3,\"why\":[\"long grep\"]}}");
    int64_t after1 = turn_tokens(db, &t);
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
    int64_t n1 = db_scalar_i64(db, "SELECT json_extract(data,'$.turn_edits[1].replaced[0].replacement')"
                                   " FROM entries WHERE id=?", t.fin, -1);
    int64_t note = db_scalar_i64(db, "SELECT json_extract(data,'$.turn_edits[1].notes[0]')"
                                     " FROM entries WHERE id=?", t.fin, -1);
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
             (long long)after1, (long long)turn_tokens(db, &t), (long long)t.fin);
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
    printf("  PASS test_record\n");
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
    expect_json(db, in, "(SELECT group_concat(key, ',') FROM json_each(?1,'$.history[0]'))",
                "turn_id,tokens,user_messages,notes,iterations,final");
    IN("$.history[0].turn_id", "1");
    expect_id_at(db, in, "$.history[0].tokens.turn", db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=? AND turn_id=1", sid, -1));
    expect_id_at(db, in, "$.history[0].tokens.tools", db_scalar_i64(db,
        "SELECT SUM(token_estimate) FROM entries WHERE session_id=? AND turn_id=1"
        " AND type IN ('tool_call','tool_result')", sid, -1));
    expect_id_at(db, in, "$.history[0].user_messages[0].entry_id", u1);
    IN("$.history[0].user_messages[0].text", "first");
    LEN("$.history[0].notes", "0");
    LEN("$.history[0].iterations", "1");
    expect_id_at(db, in, "$.history[0].iterations[0].iteration_id", it1);
    expect_id_at(db, in, "$.history[0].iterations[0].entry_id", a1);
    IN("$.history[0].iterations[0].text", "looking");
    LEN("$.history[0].iterations[0].calls", "2");
    IN("$.history[0].iterations[0].calls[0].call_id", "c1");
    IN("$.history[0].iterations[0].calls[0].args.n", "1");
    expect_id_at(db, in, "$.history[0].iterations[0].calls[0].result.entry_id", old_r);
    IN("$.history[0].iterations[0].calls[0].result.text", "OLD");
    IN("$.history[0].iterations[0].calls[1].call_id", "only1");
    expect_id_at(db, in, "$.history[0].final.entry_id", f1);
    IN("$.history[0].final.text", "answer one");
    IN("$.history[1].turn_id", "2");
    expect_id_at(db, in, "$.history[1].user_messages[0].entry_id", u2);
    LEN("$.history[1].iterations", "0");
    expect_json(db, in, "json_type(?1,'$.history[1].final')", "null");
    free(in);

    /* Edits stop at the finished turn. A call only an earlier turn made is
     * not there to name; an earlier turn's entry is no place for a note. */
    char r[160];
    const TurnEditResult NO = TURN_EDIT_REJECTED;
    expect_untouched(db, sid, E(DROP("only1")), NO,
                     "drop: \"only1\" is not a tool call of this turn");
    expect_untouched(db, sid, E("{\"op\":\"replace\",\"call_id\":\"only1\",\"text\":\"x\"}"),
                     NO, "replace: \"only1\" is not a tool call of this turn");
    int64_t not_here[] = { a1, f1, u1, u2, boom };
    for (size_t k = 0; k < sizeof(not_here) / sizeof(not_here[0]); k++) {
        snprintf(r, sizeof(r), E("{\"op\":\"note\",\"text\":\"n\",\"before\":%lld}"),
                 (long long)not_here[k]);
        expect_untouched(db, sid, r, NO, "is not the first entry of an iteration");
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
    IN("$.turn_id", "3");
    expect_id_at(db, in, "$.final.entry_id", f_next);
    LEN("$.history", "2");
    IN("$.history[0].final.text", "hello");
    IN("$.history[1].turn_id", "2");
    expect_json(db, in,
        "(SELECT group_concat(json_extract(c.value,'$.call_id'), ',')"
        "  FROM json_each(?1,'$.history[1].iterations') i, json_each(i.value,'$.calls') c)",
        "c1,c3,c4");
    IN("$.history[1].iterations[0].calls[0].result.text", "SHORTER");
    expect_json(db, in, "json_extract(?1,'$.history[1].iterations[0].calls[0].result.entry_id')"
                        " > json_extract(?1,'$.history[1].final.entry_id')", "1");
    /* A kept subset of a batch lost its reasoning row: the iteration now
     * opens at its assistant message. */
    expect_id_at(db, in, "$.history[1].iterations[0].entry_id", t.a1);
    LEN("$.history[1].notes", "1");
    IN("$.history[1].notes[0].text", "tidied");
    expect_id_at(db, in, "$.history[1].notes[0].before_iteration", t.it3);
    expect_id_at(db, in, "$.history[1].final.entry_id", t.fin);
    expect_id_at(db, in, "$.history[1].tokens.turn", turn_tokens(db, &t));
    free(in);

    /* Compaction, as llm_compaction does it: the branch keeps its first
     * entry, then the summary, then whole turns. History is that branch —
     * what is left of the first turn, the summary where it stands, and
     * nothing of what it replaced. */
    int64_t u_last = add_user(db, t.sid, "last");
    int64_t f_last = add_typed(db, t.sid, db_next_iteration_id(db, t.sid),
                               "assistant_message", "bye", NULL, NULL);
    int64_t u_next = db_scalar_i64(db, "SELECT parent_id FROM entries WHERE id=?", f_next, -1);
    int64_t cmp = entry_compact(db, t.sid, t.u0, u_next, "SUMMARY of two turns");
    assert(cmp > 0);
    in = input_all(db, t.sid);
    assert(in);
    IN("$.turn_id", "4");
    expect_id_at(db, in, "$.user_messages[0].entry_id", u_last);
    expect_id_at(db, in, "$.final.entry_id", f_last);
    LEN("$.history", "3");
    IN("$.history[0].turn_id", "1");
    expect_id_at(db, in, "$.history[0].user_messages[0].entry_id", t.u0);
    expect_json(db, in, "json_type(?1,'$.history[0].final')", "null");
    /* The summary carries the turn_id of the entry it follows, and is still
     * no part of that turn. */
    expect_id_at(db, in, "$.history[0].tokens.turn",
                 db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE id=?", t.u0, -1));
    expect_json(db, in, "(SELECT group_concat(key, ',') FROM json_each(?1,'$.history[1]'))",
                "summary");
    expect_id_at(db, in, "$.history[1].summary.entry_id", cmp);
    IN("$.history[1].summary.text", "SUMMARY of two turns");
    expect_id_at(db, in, "$.history[1].summary.tokens",
                 db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE id=?", cmp, -1));
    IN("$.history[2].turn_id", "3");
    IN("$.history[2].final.text", "sure");
    expect_json(db, in, "instr(?1,'SHORTER') + instr(?1,'tidied')", "0");
    free(in);

    /* A summary in the middle of the branch (session_condense names a range
     * of turns) stands between the turns on either side of it. */
    int64_t mid = session_create(db, "mid", "default", -1, 0);
    int64_t m1 = add_plain_turn(db, mid, "a", "A");
    add_plain_turn(db, mid, "b", "B");
    add_plain_turn(db, mid, "c", "C");
    int64_t m4 = add_plain_turn(db, mid, "d", "D");
    int64_t m4u = db_scalar_i64(db, "SELECT parent_id FROM entries WHERE id=?", m4, -1);
    int64_t mc = entry_compact(db, mid, m1, m4u, "b and c");
    assert(mc > 0);
    add_plain_turn(db, mid, "e", "E");
    in = input_all(db, mid);
    assert(in);
    IN("$.final.text", "E");
    LEN("$.history", "3");
    IN("$.history[0].final.text", "A");
    expect_id_at(db, in, "$.history[1].summary.entry_id", mc);
    IN("$.history[2].final.text", "D");
    free(in);

    #undef IN
    #undef LEN
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_history\n");
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
     * finished turn. Each row is priced at its stored text + 128, the head at 512. */
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
    assert(entry_compact(db, cs, u0, u4, sum) > 0);
    const int TURN0 = 2 + 4 + 256, TURN = 2 + 1000 + 256, SUMMARY = 300 + 128,
              HEAD = 512;

    in = input_all(db, cs);
    TURN_IDS("1,S,3,4,5");
    free(in);
    /* Room for the three turns and the summary, not for the long remnant of
     * turn 1, which is older than the summary: it goes first. */
    int max = HEAD + TURN0 + 3 * TURN + SUMMARY + 1000;
    in = input_as(db, cs, TURN_EDIT_HISTORY_ALL, 1, max, NULL);
    assert(in && (int)strlen(in) <= max);
    TURN_IDS("S,3,4,5");
    IN("$.history_omitted", "1");
    IN("$.input.history", "all");
    free(in);
    /* Room for two turns: the summary goes with everything older than them,
     * and only turns are counted as left out. */
    max = HEAD + TURN0 + 2 * TURN + 500;
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
    in = input_as(db, cs, TURN_EDIT_HISTORY_ALL, 1, 1500, &too_big);
    assert(in && strlen(in) <= 1500 && too_big == 0);
    LEN("$.history", "0");
    IN("$.history_omitted", "4");
    IN("$.final.text", "done");
    free(in);
    /* The finished turn alone over the limit: nothing is built, and the
     * caller is told why. */
    assert(input_as(db, cs, 0, 1, 600, &too_big) == NULL && too_big == 1);
    assert(input_as(db, cs, TURN_EDIT_HISTORY_ALL, 1, 600, &too_big) == NULL && too_big == 1);
    /* No finished turn is not "too big". */
    add_user(db, cs, "pending");
    assert(input_as(db, cs, 0, 1, TURN_EDIT_INPUT_MAX_BYTES, &too_big) == NULL &&
           too_big == 0);
    free(k2); free(x); free(sum);

    /* max_bytes bounds the text handed over, not the estimate. Control
     * characters are written out six bytes each (\u0001): the estimate
     * admits the earlier turn, the text does not fit, and it is left out. */
    int64_t es = session_create(db, "escape", "default", -1, 0);
    int64_t ea = add_plain_turn(db, es, "q1", "a1");
    run(db, "UPDATE entries SET content=replace(printf('%%.*c',600,'x'),'x',char(1)),"
            " content_bytes=600 WHERE id=%lld", (long long)ea);
    int64_t eb = add_plain_turn(db, es, "go", "done");
    max = HEAD + TURN0 + (2 + 600 + 256) + 200;
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

    #undef IN
    #undef LEN
    #undef TURN_IDS
    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_input_options\n");
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
    int64_t cmp = entry_compact(db, sid, u0, u, "sum");
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
    expect_shape(db, sid, G, "user:hi|user:[Summary of earlier conversation]\nsum|user:go"
                             "|model:done");
    expect_shape(db, sid, A, "user:hi|user:[Summary of earlier conversation]\nsum*|user:go"
                             "|assistant:done");

    /* Compaction after an edit: the edited turn is summarized away like any
     * other. */
    int64_t u2 = add_user(db, sid, "more");
    assert(entry_compact(db, sid, u0, u2, "sum2") > 0);
    expect_shape(db, sid, O, "user:hi|user:[Summary of earlier conversation]\nsum2|user:more");

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_turn_edges\n");
}

int main(void) {
    TEST_INIT();
    printf("test_turn_edit:\n");
    test_input();
    test_rejections();
    test_drop_one_of_batch();
    test_drop_whole_iteration();
    test_replace();
    test_notes();
    test_record();
    test_history();
    test_input_options();
    test_turn_edges();
    printf("All turn edit tests passed.\n");
    return 0;
}
