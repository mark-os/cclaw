#define _POSIX_C_SOURCE 200809L
#include "turn_review.h"
#include "buf.h"
#include "db.h"
#include "log.h"
#include "secret_interp.h"
#include "secret_store.h"
#include "types.h"
#include "unicode_normalize.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Lines ─────────────────────────────────────────────────────────
 *
 * One definition, used for the numbers the reviewer is shown and for the
 * ranges it sends back: a result's lines are its content split on '\n'. A
 * final '\n' ends the last line rather than opening an empty one, and empty
 * content has no lines. Line numbers start at 1. */

static int line_count(const char *s, size_t len) {
    if (len == 0) return 0;
    int n = 1;
    for (size_t i = 0; i + 1 < len; i++)
        if (s[i] == '\n') n++;
    return n;
}

/* The line starting at *pos: its start and length (without the '\n'), with
 * *pos moved to the next line. NULL once the content is spent. */
static const char *line_next(const char *s, size_t len, size_t *pos, size_t *linelen) {
    if (*pos >= len) return NULL;
    const char *start = s + *pos;
    const char *nl = memchr(start, '\n', len - *pos);
    *linelen = nl ? (size_t)(nl - start) : len - *pos;
    *pos += *linelen + 1;
    return start;
}

/* Every line prefixed "<n>| " — what the reviewer reads ranges off. */
static char *lines_numbered(const char *s, size_t len) {
    Buf b = {0};
    size_t pos = 0, ll;
    const char *line;
    for (int n = 1; (line = line_next(s, len, &pos, &ll)) != NULL; n++) {
        if (n > 1) buf_append_char(&b, '\n');
        buf_appendf(&b, "%d| ", n);
        buf_append(&b, line, ll);
    }
    if (!b.data) buf_append_str(&b, "");
    return buf_take(&b);
}

typedef struct { int64_t a, b; } Range;

/* Start the excerpt's next line. */
static void excerpt_line(Buf *b, int *lines) {
    if ((*lines)++) buf_append_char(b, '\n');
}

static void append_omitted(Buf *b, int *lines, int count, int64_t original_id) {
    if (count <= 0) return;
    excerpt_line(b, lines);
    buf_appendf(b, TURN_REVIEW_OMIT_FMT, count, (long long)original_id);
}

/* The kept ranges in order, each omitted run — leading and trailing ones
 * included — replaced by one marker line. Ranges are already validated:
 * within 1..total, ascending, not overlapping. */
static char *lines_excerpt(const char *s, size_t len, int total, const Range *keep,
                           int nkeep, int64_t original_id) {
    Buf b = {0};
    size_t pos = 0, ll;
    const char *line;
    int r = 0, out_lines = 0, last_kept = 0;
    for (int n = 1; (line = line_next(s, len, &pos, &ll)) != NULL; n++) {
        while (r < nkeep && n > keep[r].b) r++;
        if (r >= nkeep || n < keep[r].a) continue;
        append_omitted(&b, &out_lines, n - last_kept - 1, original_id);
        excerpt_line(&b, &out_lines);
        buf_append(&b, line, ll);
        last_kept = n;
    }
    append_omitted(&b, &out_lines, total - last_kept, original_id);
    if (!b.data) buf_append_str(&b, "");
    return buf_take(&b);
}

static void fn_lines(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *s = (const char *)sqlite3_value_text(argv[0]);
    sqlite3_result_int(ctx, s ? line_count(s, (size_t)sqlite3_value_bytes(argv[0])) : 0);
}

static void fn_numbered(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc;
    const char *s = (const char *)sqlite3_value_text(argv[0]);
    char *out = lines_numbered(s ? s : "", s ? (size_t)sqlite3_value_bytes(argv[0]) : 0);
    if (out) sqlite3_result_text(ctx, out, -1, free);
    else sqlite3_result_error_nomem(ctx);
}

/* The input query calls the two functions above so that the line rule lives
 * in C once. Registering replaces any earlier registration on the handle. */
static int register_line_functions(sqlite3 *db) {
    int flags = SQLITE_UTF8 | SQLITE_DETERMINISTIC;
    return sqlite3_create_function(db, "turn_review_lines", 1, flags, NULL,
                                   fn_lines, NULL, NULL) == SQLITE_OK &&
           sqlite3_create_function(db, "turn_review_numbered", 1, flags, NULL,
                                   fn_numbered, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/* ── The finished turn ─────────────────────────────────────────────
 *
 * The newest turn on the active branch: walk leaf→root for as long as the
 * turn_id stays the leaf's. ?1 = session id. lvl 0 is the leaf. */
#define SQL_TURN \
    "turn(id, parent_id, turn_id, lvl) AS (" \
    "  SELECT id, parent_id, turn_id, 0 FROM entries" \
    "   WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1) AND session_id=?1" \
    "  UNION ALL" \
    "  SELECT e.id, e.parent_id, e.turn_id, t.lvl+1" \
    "    FROM entries e JOIN turn t ON e.id=t.parent_id" \
    "   WHERE e.turn_id=t.turn_id AND e.session_id=?1)"

/* May a call be dropped? Both read off the tool_call entry aliased C.
 *
 * Droppable is a fact about the tool (tools.droppable — the built-in recipe
 * flag, or an extension manifest's word; a tool with no row is protected),
 * with one exception that is a fact about the call: save_secret turns any
 * retrieving tool into one that stored a credential, and that is an effect.
 *
 * Finished: tool_calls is workflow state, and anything but 'done' means
 * something is still going to answer this call (a background job). A call
 * with no row at all was answered at ingest (unparseable arguments). */
#define SQL_CALL_DROPPABLE(C) \
    "(COALESCE((SELECT tl.droppable FROM tools tl WHERE tl.name=" C ".tool_name),0)=1" \
    " AND COALESCE(CASE WHEN json_valid(" C ".content)" \
    "   THEN json_extract(" C ".content,'$.save_secret') END,'')='')"
#define SQL_CALL_UNFINISHED(C) \
    "(SELECT tc.status FROM tool_calls tc" \
    "  WHERE tc.entry_id=" C ".id AND tc.status<>'done' LIMIT 1)"

/* The result answering call A, among the turn's parts. Paired by call id
 * alone: a result written outside the dispatcher (a sub-agent's answer, an
 * approval notice) is appended under an iteration id of its own, not its
 * call's. */
#define SQL_RESULT_OF_A \
    " FROM part r WHERE r.type='tool_result' AND r.tool_call_id=a.tool_call_id" \
    " ORDER BY r.lvl DESC LIMIT 1"

/* ?1 = session id, ?2 = note cap. No row when the leaf is not an assistant
 * message. The last iteration is the answer, reported apart as `final`. */
static const char SQL_INPUT[] =
    "WITH RECURSIVE " SQL_TURN ","
    " part AS MATERIALIZED ("
    "  SELECT t.lvl, e.id, e.type, e.iteration_id, e.turn_id, e.content,"
    "         e.tool_call_id, e.tool_name, e.is_error, e.token_estimate"
    "    FROM turn t JOIN entries e ON e.id=t.id)"
    "SELECT json_object("
    "  'event','turnSettled',"
    "  'session_id',?1,"
    "  'agent',(SELECT agent_name FROM sessions WHERE id=?1),"
    "  'turn_id',f.turn_id,"
    "  'user_messages',(SELECT json_group_array("
    "      json_object('entry_id',u.id,'text',COALESCE(u.content,'')) ORDER BY u.lvl DESC)"
    "    FROM part u WHERE u.type IN ('user_message','system')),"
    "  'iterations',(SELECT json_group_array(json(i.obj) ORDER BY i.lvl DESC) FROM ("
    "    SELECT MAX(a.lvl) AS lvl, json_object("
    "      'iteration_id',a.iteration_id,"
    "      'text',COALESCE(MAX(CASE WHEN a.type='assistant_message' THEN a.content END),''),"
    "      'calls',json_group_array(json(json_object("
    "        'call_id',a.tool_call_id,"
    "        'tool',a.tool_name,"
    "        'args',CASE WHEN json_valid(a.content) THEN json(a.content) ELSE a.content END,"
    "        'protected',json(CASE WHEN " SQL_CALL_DROPPABLE("a")
    "            AND " SQL_CALL_UNFINISHED("a") " IS NULL THEN 'false' ELSE 'true' END),"
    "        'is_error',json(CASE WHEN COALESCE((SELECT r.is_error" SQL_RESULT_OF_A "),0)"
    "            THEN 'true' ELSE 'false' END),"
    "        'result',json((SELECT json_object("
    "            'entry_id',r.id,"
    "            'lines',turn_review_lines(r.content),"
    "            'tokens',r.token_estimate,"
    "            'text',turn_review_numbered(r.content))" SQL_RESULT_OF_A "))"
    "        )) ORDER BY a.lvl DESC) FILTER (WHERE a.type='tool_call')) AS obj"
    "    FROM part a WHERE a.type IN ('assistant_message','tool_call') AND a.lvl > 0"
    "    GROUP BY a.iteration_id) i),"
    "  'final',json_object('entry_id',f.id,'text',COALESCE(f.content,'')),"
    "  'tokens',json_object("
    "    'turn',(SELECT COALESCE(SUM(token_estimate),0) FROM part),"
    "    'tool_traffic',(SELECT COALESCE(SUM(token_estimate),0) FROM part"
    "                     WHERE type IN ('tool_call','tool_result'))),"
    "  'limits',json_object('note_max_chars',?2))"
    " FROM part f WHERE f.lvl=0 AND f.type='assistant_message';";

char *turn_review_input(sqlite3 *db, int64_t session_id) {
    if (!db || register_line_functions(db) != 0) return NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL_INPUT, -1, &st, NULL) != SQLITE_OK) {
        LOG_ERROR_("turn review input: %s", sqlite3_errmsg(db));
        return NULL;
    }
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_int(st, 2, TURN_REVIEW_NOTE_MAX_CHARS);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

int turn_review_measure(sqlite3 *db, int64_t session_id, int *turn_tokens,
                        int *tool_tokens) {
    if (turn_tokens) *turn_tokens = 0;
    if (tool_tokens) *tool_tokens = 0;
    if (!db) return -1;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE " SQL_TURN
            " SELECT COUNT(*), COALESCE(SUM(e.token_estimate),0),"
            "   COALESCE(SUM(e.token_estimate)"
            "     FILTER (WHERE e.type IN ('tool_call','tool_result')),0)"
            " FROM turn t JOIN entries e ON e.id=t.id;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, session_id);
    int rc = -1;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) > 0) {
        if (turn_tokens) *turn_tokens = sqlite3_column_int(st, 1);
        if (tool_tokens) *tool_tokens = sqlite3_column_int(st, 2);
        rc = 0;
    }
    sqlite3_finalize(st);
    return rc;
}

/* ── The reply ─────────────────────────────────────────────────────*/

typedef struct {
    char *call_id;
    Range *keep;
    int nkeep;
    int result_row;     /* index of the result it replaces, once resolved */
    int total, kept;    /* lines */
    char *excerpt;
    int64_t new_id;
} Trim;

typedef struct {
    char **drop;
    int ndrop;
    Trim *trim;
    int ntrim;
    char *note;         /* NULL = none given */
} Reply;

static void reply_free(Reply *r) {
    for (int i = 0; i < r->ndrop; i++) free(r->drop[i]);
    free(r->drop);
    for (int i = 0; i < r->ntrim; i++) {
        free(r->trim[i].call_id);
        free(r->trim[i].keep);
        free(r->trim[i].excerpt);
    }
    free(r->trim);
    free(r->note);
    memset(r, 0, sizeof(*r));
}

static TurnReviewResult fail(char **err_out, TurnReviewResult rc, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static TurnReviewResult fail(char **err_out, TurnReviewResult rc, const char *fmt, ...) {
    if (err_out && !*err_out) {
        char msg[320];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        *err_out = strdup(msg);
    }
    return rc;
}

/* First thing wrong with the reply's shape, or NULL. ?1 = the reply, already
 * known to be a JSON object. Every arm that reads into a value is guarded by
 * the arm that checks its type: COALESCE stops at the first hit, and CASE
 * keeps json functions off values that are not JSON. */
static const char SQL_REPLY_SHAPE[] =
    "SELECT COALESCE("
    " (SELECT 'unknown key \"' || key || '\"' FROM json_each(?1)"
    "   WHERE key NOT IN ('drop','trim','note') LIMIT 1),"
    " (SELECT 'key \"' || key || '\" appears twice' FROM json_each(?1)"
    "   GROUP BY key HAVING COUNT(*) > 1 LIMIT 1),"
    " CASE WHEN json_type(?1,'$.drop') <> 'array'"
    "      THEN '\"drop\" must be an array of call ids' END,"
    " (SELECT 'drop[' || key || '] must be a call id string'"
    "   FROM json_each(?1,'$.drop') WHERE type <> 'text' LIMIT 1),"
    " CASE WHEN json_type(?1,'$.trim') <> 'array' THEN '\"trim\" must be an array' END,"
    " (SELECT 'trim[' || t.key || '] must be {\"call_id\": string, \"keep\": array}'"
    "   FROM json_each(?1,'$.trim') t"
    "   WHERE CASE WHEN t.type <> 'object' THEN 1 ELSE"
    "     json_type(t.value,'$.call_id') IS NOT 'text'"
    "     OR json_type(t.value,'$.keep') IS NOT 'array'"
    "     OR EXISTS (SELECT 1 FROM json_each(t.value) k"
    "                 WHERE k.key NOT IN ('call_id','keep'))"
    "     OR (SELECT COUNT(*) FROM json_each(t.value)) <> 2 END LIMIT 1),"
    " (SELECT 'trim[' || t.key || '].keep[' || r.key || '] must be a pair of integers'"
    "   FROM json_each(?1,'$.trim') t, json_each(t.value,'$.keep') r"
    "   WHERE CASE WHEN r.type <> 'array' THEN 1 ELSE"
    "     json_array_length(r.value) <> 2"
    "     OR json_type(r.value,'$[0]') IS NOT 'integer'"
    "     OR json_type(r.value,'$[1]') IS NOT 'integer' END LIMIT 1),"
    " CASE WHEN json_type(?1,'$.note') <> 'text' THEN '\"note\" must be a string' END);";

static char *reply_text(sqlite3 *db, const char *sql, const char *reply, int *ok) {
    sqlite3_stmt *st;
    *ok = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, reply, -1, SQLITE_STATIC);
    char *out = NULL;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
        *ok = 1;
    } else if (rc == SQLITE_DONE) {
        *ok = 1;
    }
    sqlite3_finalize(st);
    return out;
}

/* Parse the reply into C. Reads no table: only JSON functions over ?1. */
static TurnReviewResult reply_parse(sqlite3 *db, const char *reply, Reply *out,
                                    char **err_out) {
    int ok;
    char *kind = reply_text(db,
        "SELECT CASE WHEN json_valid(?1) THEN json_type(?1) END;", reply, &ok);
    if (!ok) return fail(err_out, TURN_REVIEW_ERROR, "reply check failed: %s",
                         sqlite3_errmsg(db));
    int is_object = kind && strcmp(kind, "object") == 0;
    free(kind);
    if (!is_object)
        return fail(err_out, TURN_REVIEW_REJECTED, "reply is not a JSON object");

    char *bad = reply_text(db, SQL_REPLY_SHAPE, reply, &ok);
    if (!ok) return fail(err_out, TURN_REVIEW_ERROR, "reply check failed: %s",
                         sqlite3_errmsg(db));
    if (bad) {
        fail(err_out, TURN_REVIEW_REJECTED, "%s", bad);
        free(bad);
        return TURN_REVIEW_REJECTED;
    }

    out->note = reply_text(db, "SELECT json_extract(?1,'$.note');", reply, &ok);

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT (SELECT COUNT(*) FROM json_each(?1,'$.drop')),"
            "       (SELECT COUNT(*) FROM json_each(?1,'$.trim'));",
            -1, &st, NULL) != SQLITE_OK)
        return fail(err_out, TURN_REVIEW_ERROR, "reply read failed");
    sqlite3_bind_text(st, 1, reply, -1, SQLITE_STATIC);
    int ndrop = 0, ntrim = 0;
    if (sqlite3_step(st) == SQLITE_ROW) {
        ndrop = sqlite3_column_int(st, 0);
        ntrim = sqlite3_column_int(st, 1);
    }
    sqlite3_finalize(st);
    out->drop = calloc((size_t)ndrop + 1, sizeof(*out->drop));
    out->trim = calloc((size_t)ntrim + 1, sizeof(*out->trim));
    if (!out->drop || !out->trim) return fail(err_out, TURN_REVIEW_ERROR, "out of memory");

    if (sqlite3_prepare_v2(db, "SELECT value FROM json_each(?1,'$.drop') ORDER BY key;",
                           -1, &st, NULL) != SQLITE_OK)
        return fail(err_out, TURN_REVIEW_ERROR, "reply read failed");
    sqlite3_bind_text(st, 1, reply, -1, SQLITE_STATIC);
    while (out->ndrop < ndrop && sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (!(out->drop[out->ndrop] = strdup(v ? v : ""))) break;
        out->ndrop++;
    }
    sqlite3_finalize(st);

    /* One row per range, grouped by trim; a trim with an empty keep list
     * still yields its row (LEFT JOIN) so the caller can refuse it by name. */
    if (sqlite3_prepare_v2(db,
            "SELECT t.key, json_extract(t.value,'$.call_id'),"
            "       json_array_length(t.value,'$.keep'),"
            "       json_extract(r.value,'$[0]'), json_extract(r.value,'$[1]')"
            " FROM json_each(?1,'$.trim') t"
            " LEFT JOIN json_each(t.value,'$.keep') r"
            " ORDER BY t.key, r.key;", -1, &st, NULL) != SQLITE_OK)
        return fail(err_out, TURN_REVIEW_ERROR, "reply read failed");
    sqlite3_bind_text(st, 1, reply, -1, SQLITE_STATIC);
    int oom = 0;
    while (!oom && sqlite3_step(st) == SQLITE_ROW) {
        int ti = sqlite3_column_int(st, 0);
        if (ti < 0 || ti >= ntrim) continue;
        Trim *t = &out->trim[ti];
        if (!t->call_id) {
            const char *v = (const char *)sqlite3_column_text(st, 1);
            int n = sqlite3_column_int(st, 2);
            t->call_id = strdup(v ? v : "");
            t->keep = calloc((size_t)n + 1, sizeof(*t->keep));
            t->result_row = -1;
            if (!t->call_id || !t->keep) { oom = 1; break; }
            if (ti + 1 > out->ntrim) out->ntrim = ti + 1;
        }
        if (sqlite3_column_type(st, 3) == SQLITE_NULL) continue;
        t->keep[t->nkeep].a = sqlite3_column_int64(st, 3);
        t->keep[t->nkeep].b = sqlite3_column_int64(st, 4);
        t->nkeep++;
    }
    sqlite3_finalize(st);
    if (oom || out->ndrop != ndrop || out->ntrim != ntrim)
        return fail(err_out, TURN_REVIEW_ERROR, "out of memory");
    return TURN_REVIEW_NOTHING;
}

static int utf8_chars(const char *s) {
    int n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

/* The note enters every later request, so it passes what any other text
 * entering context passes (inbox_insert_scanned's chain): invisible
 * characters stripped, then stored secret values and scanner rules looked
 * for. Here a hit refuses the reply rather than masking the note — a
 * reviewer that copied a credential out of a tool result got something
 * wrong, and half a note is not what it meant to leave. */
static TurnReviewResult note_check(sqlite3 *db, Reply *r, char **err_out) {
    if (!r->note) return TURN_REVIEW_NOTHING;
    char *clean = unicode_strip_invisible(r->note, strlen(r->note), NULL);
    if (!clean) return fail(err_out, TURN_REVIEW_ERROR, "out of memory");
    free(r->note);
    r->note = clean;

    int chars = utf8_chars(r->note);
    if (chars > TURN_REVIEW_NOTE_MAX_CHARS)
        return fail(err_out, TURN_REVIEW_REJECTED,
                    "note is %d characters; the limit is %d", chars,
                    TURN_REVIEW_NOTE_MAX_CHARS);

    size_t snap_n = 0;
    ShellSecret *snap = secrets_snapshot(db, NULL, 0, &snap_n);
    char *masked = tool_result_postprocess(r->note, snap, snap_n);
    secrets_snapshot_free(snap, snap_n);
    if (masked) {      /* non-NULL only when something was masked or redacted */
        free(masked);
        return fail(err_out, TURN_REVIEW_REJECTED, "note contains a secret");
    }
    return TURN_REVIEW_NOTHING;
}

/* ── The turn, in memory for one apply ─────────────────────────────*/

typedef enum { ROW_OTHER, ROW_ASSISTANT, ROW_REASONING, ROW_CALL, ROW_RESULT,
               ROW_REVIEW } RowKind;

typedef struct {
    int64_t id, parent_id, iteration_id;
    RowKind kind;
    char *call_id;
    int tokens;
    int droppable;          /* calls */
    char unfinished[16];    /* calls: the tool_calls status when not 'done' */
    int remove;             /* leaves the branch */
    int trim;               /* results: index into Reply.trim, or -1 */
} Row;

typedef struct {
    Row *rows;              /* root→leaf */
    int n;
    int64_t turn_id;
    int64_t leaf_id;
    int64_t before_id;      /* the entry the turn's first row hangs off */
} Turn;

static void turn_free(Turn *t) {
    for (int i = 0; i < t->n; i++) free(t->rows[i].call_id);
    free(t->rows);
    memset(t, 0, sizeof(*t));
}

static RowKind row_kind(const char *type) {
    if (!type) return ROW_OTHER;
    if (strcmp(type, "assistant_message") == 0) return ROW_ASSISTANT;
    if (strcmp(type, "reasoning") == 0) return ROW_REASONING;
    if (strcmp(type, "tool_call") == 0) return ROW_CALL;
    if (strcmp(type, "tool_result") == 0) return ROW_RESULT;
    if (strcmp(type, "turn_review") == 0) return ROW_REVIEW;
    return ROW_OTHER;
}

static int turn_load(sqlite3 *db, int64_t session_id, Turn *out) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "WITH RECURSIVE " SQL_TURN
            " SELECT e.id, e.parent_id, COALESCE(e.iteration_id,0), e.type,"
            "   e.tool_call_id, COALESCE(e.token_estimate,0), e.turn_id,"
            "   CASE WHEN e.type='tool_call' THEN " SQL_CALL_DROPPABLE("e") " END,"
            "   CASE WHEN e.type='tool_call' THEN " SQL_CALL_UNFINISHED("e") " END"
            " FROM turn t JOIN entries e ON e.id=t.id ORDER BY t.lvl DESC;",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, session_id);
    int cap = 64, rc = 0;
    out->rows = calloc((size_t)cap, sizeof(Row));
    while (out->rows && sqlite3_step(st) == SQLITE_ROW) {
        if (out->n == cap) {
            cap *= 2;
            Row *tmp = realloc(out->rows, (size_t)cap * sizeof(Row));
            if (!tmp) { rc = -1; break; }
            out->rows = tmp;
        }
        Row *r = &out->rows[out->n];
        memset(r, 0, sizeof(*r));
        r->id = sqlite3_column_int64(st, 0);
        r->parent_id = sqlite3_column_int64(st, 1);
        r->iteration_id = sqlite3_column_int64(st, 2);
        r->kind = row_kind((const char *)sqlite3_column_text(st, 3));
        const char *cid = (const char *)sqlite3_column_text(st, 4);
        r->call_id = cid ? strdup(cid) : NULL;
        r->tokens = sqlite3_column_int(st, 5);
        out->turn_id = sqlite3_column_int64(st, 6);
        r->droppable = sqlite3_column_int(st, 7);
        const char *status = (const char *)sqlite3_column_text(st, 8);
        if (status) snprintf(r->unfinished, sizeof(r->unfinished), "%s", status);
        r->trim = -1;
        out->n++;
        if (cid && !r->call_id) { rc = -1; break; }
    }
    sqlite3_finalize(st);
    if (!out->rows || rc != 0) return -1;
    if (out->n > 0) {
        out->before_id = out->rows[0].parent_id;
        out->leaf_id = out->rows[out->n - 1].id;
    }
    return 0;
}

/* The one tool_call row with this id; -1 none, -2 more than one. */
static int find_call(const Turn *t, const char *call_id) {
    int found = -1;
    for (int i = 0; i < t->n; i++) {
        if (t->rows[i].kind != ROW_CALL || !t->rows[i].call_id ||
            strcmp(t->rows[i].call_id, call_id) != 0)
            continue;
        if (found >= 0) return -2;
        found = i;
    }
    return found;
}

/* The call must be this turn's, named once, and answered for good. */
static TurnReviewResult check_call(const Turn *t, const char *verb, const char *call_id,
                                   int *idx, char **err_out) {
    *idx = find_call(t, call_id);
    if (*idx == -1)
        return fail(err_out, TURN_REVIEW_REJECTED,
                    "%s: \"%.80s\" is not a tool call of this turn", verb, call_id);
    if (*idx == -2)
        return fail(err_out, TURN_REVIEW_REJECTED,
                    "%s: call id \"%.80s\" names more than one call in this turn",
                    verb, call_id);
    const Row *c = &t->rows[*idx];
    if (c->unfinished[0])
        return fail(err_out, TURN_REVIEW_REJECTED,
                    "%s: call \"%.80s\" is not finished (%s)", verb, call_id,
                    c->unfinished);
    return TURN_REVIEW_NOTHING;
}

/* By call id alone (see SQL_RESULT_OF_A); the call id is known to name one
 * call of the turn by the time this is asked. */
static int is_result_of(const Row *r, const Row *call) {
    return r->kind == ROW_RESULT && r->call_id && strcmp(r->call_id, call->call_id) == 0;
}

static TurnReviewResult mark_drops(Turn *t, const Reply *reply, char **err_out) {
    for (int d = 0; d < reply->ndrop; d++) {
        const char *cid = reply->drop[d];
        int ci;
        TurnReviewResult rc = check_call(t, "drop", cid, &ci, err_out);
        if (rc != TURN_REVIEW_NOTHING) return rc;
        Row *c = &t->rows[ci];
        if (c->remove)
            return fail(err_out, TURN_REVIEW_REJECTED,
                        "drop: call \"%.80s\" is listed twice", cid);
        if (!c->droppable)
            return fail(err_out, TURN_REVIEW_REJECTED,
                        "drop: call \"%.80s\" is protected and cannot be dropped", cid);
        /* Iterations are told apart by id; without one there is no saying
         * which assistant message a call belongs to. */
        if (c->iteration_id <= 0)
            return fail(err_out, TURN_REVIEW_REJECTED,
                        "drop: call \"%.80s\" has no iteration id", cid);
        c->remove = 1;
        for (int i = 0; i < t->n; i++)
            if (is_result_of(&t->rows[i], c)) t->rows[i].remove = 1;
    }
    return TURN_REVIEW_NOTHING;
}

/* Ranges: 1 <= a <= b <= total, each starting after the one before ended. */
static TurnReviewResult check_ranges(const Trim *tr, char **err_out) {
    int64_t prev_end = 0;
    for (int i = 0; i < tr->nkeep; i++) {
        int64_t a = tr->keep[i].a, b = tr->keep[i].b;
        if (a < 1 || a > b || b > tr->total)
            return fail(err_out, TURN_REVIEW_REJECTED,
                        "trim: call \"%.80s\" range [%lld,%lld] is outside 1..%d",
                        tr->call_id, (long long)a, (long long)b, tr->total);
        if (a <= prev_end)
            return fail(err_out, TURN_REVIEW_REJECTED,
                        "trim: call \"%.80s\" range [%lld,%lld] overlaps or precedes"
                        " the one before it", tr->call_id, (long long)a, (long long)b);
        prev_end = b;
    }
    return TURN_REVIEW_NOTHING;
}

/* The result row answering a call: its index, -1 none, -2 more than one. */
static int find_result(const Turn *t, const Row *call) {
    int found = -1;
    for (int i = 0; i < t->n; i++) {
        if (!is_result_of(&t->rows[i], call)) continue;
        if (found >= 0) return -2;
        found = i;
    }
    return found;
}

/* Check one trim's ranges against the result's text and build its excerpt.
 * Keeping every line asks for no change: the result stays as it is and the
 * trim is forgotten, not refused. The same goes for an excerpt no smaller
 * than what it would replace — a marker line costs more than a short line
 * it stands for. Either way tr->excerpt stays NULL. */
static TurnReviewResult trim_build(Trim *tr, const char *content, size_t len,
                                   int64_t result_id, char **err_out) {
    tr->total = line_count(content, len);
    TurnReviewResult rc = check_ranges(tr, err_out);
    if (rc != TURN_REVIEW_NOTHING) return rc;
    for (int i = 0; i < tr->nkeep; i++)
        tr->kept += (int)(tr->keep[i].b - tr->keep[i].a + 1);
    if (tr->kept == tr->total) return TURN_REVIEW_NOTHING;
    tr->excerpt = lines_excerpt(content, len, tr->total, tr->keep, tr->nkeep, result_id);
    if (!tr->excerpt) return fail(err_out, TURN_REVIEW_ERROR, "out of memory");
    if (strlen(tr->excerpt) >= len) {
        free(tr->excerpt);
        tr->excerpt = NULL;
    }
    return TURN_REVIEW_NOTHING;
}

static TurnReviewResult mark_trim(sqlite3_stmt *content_st, Turn *t, Reply *reply, int k,
                                  char **err_out) {
    Trim *tr = &reply->trim[k];
    int ci;
    TurnReviewResult rc = check_call(t, "trim", tr->call_id, &ci, err_out);
    if (rc != TURN_REVIEW_NOTHING) return rc;
    const Row *c = &t->rows[ci];
    if (c->remove)
        return fail(err_out, TURN_REVIEW_REJECTED,
                    "call \"%.80s\" is in both drop and trim", tr->call_id);
    int ri = find_result(t, c);
    if (ri < 0)
        return fail(err_out, TURN_REVIEW_REJECTED, "trim: call \"%.80s\" has %s",
                    tr->call_id, ri == -2 ? "more than one result" : "no result to trim");
    if (t->rows[ri].trim >= 0)
        return fail(err_out, TURN_REVIEW_REJECTED,
                    "trim: call \"%.80s\" is listed twice", tr->call_id);
    if (tr->nkeep == 0)
        return fail(err_out, TURN_REVIEW_REJECTED,
                    "trim: call \"%.80s\" keeps nothing — that is a drop", tr->call_id);

    sqlite3_bind_int64(content_st, 1, t->rows[ri].id);
    if (sqlite3_step(content_st) != SQLITE_ROW)
        return fail(err_out, TURN_REVIEW_ERROR, "trim: result entry unreadable");
    const char *content = (const char *)sqlite3_column_text(content_st, 0);
    size_t len = content ? (size_t)sqlite3_column_bytes(content_st, 0) : 0;
    rc = trim_build(tr, content ? content : "", len, t->rows[ri].id, err_out);
    /* Claimed even when the trim changes nothing, so a second trim of the
     * same call is still a duplicate. */
    t->rows[ri].trim = k;
    tr->result_row = ri;
    return rc;
}

static TurnReviewResult mark_trims(sqlite3 *db, Turn *t, Reply *reply, char **err_out) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT content FROM entries WHERE id=?1;", -1, &st,
                           NULL) != SQLITE_OK)
        return fail(err_out, TURN_REVIEW_ERROR, "trim: %s", sqlite3_errmsg(db));
    TurnReviewResult rc = TURN_REVIEW_NOTHING;
    for (int k = 0; k < reply->ntrim && rc == TURN_REVIEW_NOTHING; k++) {
        rc = mark_trim(st, t, reply, k, err_out);
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);
    return rc;
}

/* What a dropped call takes with it. An iteration left with no call goes
 * whole, text and reasoning included; one that keeps some of its calls loses
 * its reasoning entry, because a provider's reasoning blob is bound to the
 * full set of calls the model made and must not be replayed beside a subset.
 * The assistant message's tool_call_count is not touched: it counts the
 * legacy entries.tool_calls JSON, which typed tool_call rows never fill.
 * The turn's last row is the answer and never leaves, whatever iteration id
 * it carries. */
static void mark_iterations(Turn *t) {
    for (int i = 0; i < t->n; i++) {
        const Row *c = &t->rows[i];
        if (c->kind != ROW_CALL || !c->remove) continue;
        int left = 0;
        for (int j = 0; j < t->n; j++)
            if (t->rows[j].kind == ROW_CALL && !t->rows[j].remove &&
                t->rows[j].iteration_id == c->iteration_id)
                left++;
        for (int j = 0; j < t->n - 1; j++) {
            Row *r = &t->rows[j];
            if (r->iteration_id != c->iteration_id) continue;
            if (r->kind == ROW_REASONING ||
                (!left && (r->kind == ROW_ASSISTANT || r->kind == ROW_RESULT)))
                r->remove = 1;
        }
    }
}

/* ── Writing ───────────────────────────────────────────────────────*/

static int exec_ids(sqlite3 *db, const char *sql, int64_t a, int64_t b) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, a);
    sqlite3_bind_int64(st, 2, b);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* A copy of a tool_result carrying the excerpt, hung under `parent`. turn_id
 * is given so entries_turn_ai leaves it alone. */
static int64_t insert_trimmed(sqlite3 *db, int64_t original_id, int64_t parent,
                              const char *excerpt) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO entries (parent_id, session_id, turn_id, iteration_id, type,"
            "  part_index, role, content, tool_call_id, tool_name, is_error,"
            "  network_hosts, fence_id, token_estimate, content_bytes)"
            " SELECT ?2, session_id, turn_id, iteration_id, type, part_index, role,"
            "  ?3, tool_call_id, tool_name, is_error, network_hosts, fence_id, ?4, ?5"
            " FROM entries WHERE id=?1;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    int len = (int)strlen(excerpt);
    sqlite3_bind_int64(st, 1, original_id);
    sqlite3_bind_int64(st, 2, parent);
    sqlite3_bind_text(st, 3, excerpt, len, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, TOKEN_ESTIMATE(len));
    sqlite3_bind_int(st, 5, len);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed == 1 ? sqlite3_last_insert_rowid(db) : -1;
}

/* The review entry. Role 0: it is the runtime speaking, and every reader
 * that picks entries by role already steps over a system note — delivery
 * and the "turn ended" checks look for role 2, the human-turn test for role
 * 1, tool-result grouping and the error filter for role 3, the compaction
 * trigger for role 4. */
static int64_t insert_review(sqlite3 *db, int64_t session_id, int64_t turn_id,
                             int64_t parent, const char *note, const char *model) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO entries (parent_id, session_id, turn_id, iteration_id, type,"
            "  role, content, token_estimate, content_bytes, data)"
            " VALUES (?1, ?2, ?3, 0, 'turn_review', 0, ?4, ?5, ?6,"
            "  json_object('dropped',json('[]'),'trimmed',json('[]'),"
            "              'reviewer_model',?7));", -1, &st, NULL) != SQLITE_OK)
        return -1;
    int len = note ? (int)strlen(note) : 0;
    sqlite3_bind_int64(st, 1, parent);
    sqlite3_bind_int64(st, 2, session_id);
    sqlite3_bind_int64(st, 3, turn_id);
    if (len) sqlite3_bind_text(st, 4, note, len, SQLITE_STATIC);
    sqlite3_bind_int(st, 5, TOKEN_ESTIMATE(len));
    sqlite3_bind_int(st, 6, len);
    if (model && model[0]) sqlite3_bind_text(st, 7, model, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : -1;
}

/* Rebuild the turn's chain front to back: every row that stays hangs off the
 * row that now precedes it. A re-pointed row remembers the parent it was
 * written under — the first one only, so the chain as written can always be
 * walked back (a row that leaves the branch is not touched and still points
 * at its own original parent). Returns the number of rows on the new chain,
 * or -1. */
static int write_chain(sqlite3 *db, int64_t session_id, Turn *t, Reply *reply,
                       const char *model, int64_t *review_id) {
    /* The answer is the turn's last iteration; the review entry goes before
     * all of it, so the answer's reasoning entry stays next to it. */
    int final_start = t->n - 1;
    if (final_start > 0 && t->rows[final_start - 1].kind == ROW_REASONING &&
        t->rows[final_start - 1].iteration_id == t->rows[final_start].iteration_id)
        final_start--;

    int64_t prev = t->before_id;
    int on_chain = 0;
    for (int i = 0; i < t->n; i++) {
        Row *r = &t->rows[i];
        if (i == final_start) {
            *review_id = insert_review(db, session_id, t->turn_id, prev, reply->note,
                                       model);
            if (*review_id <= 0) return -1;
            prev = *review_id;
            on_chain++;
        }
        if (r->remove) continue;
        Trim *tr = r->trim >= 0 ? &reply->trim[r->trim] : NULL;
        if (tr && tr->excerpt) {
            tr->new_id = insert_trimmed(db, r->id, prev, tr->excerpt);
            if (tr->new_id <= 0) return -1;
            prev = tr->new_id;
            on_chain++;
            continue;
        }
        if (r->parent_id != prev &&
            exec_ids(db, "UPDATE entries SET"
                         " original_parent_id=COALESCE(original_parent_id, parent_id),"
                         " parent_id=?2 WHERE id=?1;", r->id, prev) != 0)
            return -1;
        prev = r->id;
        on_chain++;
    }
    return on_chain;
}

/* Complete the review entry's record once the chain is written: what left
 * the branch (in chain order), what was replaced, and the sizes. */
static int write_record(sqlite3 *db, int64_t session_id, const Turn *t,
                        const Reply *reply, int64_t review_id, int before) {
    for (int i = 0; i < t->n; i++)
        if (t->rows[i].remove &&
            exec_ids(db, "UPDATE entries SET data=json_insert(data,'$.dropped[#]',?2)"
                         " WHERE id=?1;", review_id, t->rows[i].id) != 0)
            return -1;

    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET data=json_insert(data,'$.trimmed[#]',"
            "  json_object('original',?2,'replacement',?3,'lines_kept',?4,"
            "              'lines_total',?5)) WHERE id=?1;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    for (int k = 0; k < reply->ntrim; k++) {
        const Trim *tr = &reply->trim[k];
        if (!tr->excerpt) continue;
        sqlite3_bind_int64(st, 1, review_id);
        sqlite3_bind_int64(st, 2, t->rows[tr->result_row].id);
        sqlite3_bind_int64(st, 3, tr->new_id);
        sqlite3_bind_int(st, 4, tr->kept);
        sqlite3_bind_int(st, 5, tr->total);
        if (sqlite3_step(st) != SQLITE_DONE) { sqlite3_finalize(st); return -1; }
        sqlite3_reset(st);
    }
    sqlite3_finalize(st);

    int after = 0;
    if (turn_review_measure(db, session_id, &after, NULL) != 0) return -1;
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET data=json_set(data,'$.tokens_before',?2,"
            "  '$.tokens_after',?3) WHERE id=?1;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, review_id);
    sqlite3_bind_int(st, 2, before);
    sqlite3_bind_int(st, 3, after);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* The turn is finished and not yet reviewed: its leaf is the answer and no
 * call is waiting to run. *before gets the turn's size. */
static TurnReviewResult check_turn(const Turn *t, int *before, char **err_out) {
    if (t->n == 0 || t->rows[t->n - 1].kind != ROW_ASSISTANT)
        return fail(err_out, TURN_REVIEW_REJECTED,
                    "the session's newest entry is not an assistant message —"
                    " no finished turn to review");
    for (int i = 0; i < t->n; i++) {
        const Row *r = &t->rows[i];
        *before += r->tokens;
        if (r->kind == ROW_REVIEW)
            return fail(err_out, TURN_REVIEW_REJECTED,
                        "turn %lld was already reviewed (entry %lld)",
                        (long long)t->turn_id, (long long)r->id);
        if (strcmp(r->unfinished, "pending") == 0 || strcmp(r->unfinished, "running") == 0)
            return fail(err_out, TURN_REVIEW_REJECTED,
                        "call \"%.80s\" is still %s — the turn is not finished",
                        r->call_id ? r->call_id : "?", r->unfinished);
    }
    return TURN_REVIEW_NOTHING;
}

/* Proof before COMMIT: walked from the same leaf, the turn is one chain of
 * exactly the rows meant to be on it, hanging off the same entry. */
static int chain_sound(sqlite3 *db, int64_t session_id, const Turn *was, int expect) {
    Turn now = {0};
    int sound = turn_load(db, session_id, &now) == 0 && now.n == expect &&
                now.leaf_id == was->leaf_id && now.before_id == was->before_id;
    turn_free(&now);
    return sound;
}

/* Everything that needs the turn: runs inside the write transaction, so what
 * is checked is what is edited. */
static TurnReviewResult apply_locked(sqlite3 *db, int64_t session_id, Reply *reply,
                                     const char *model, char **err_out) {
    Turn t = {0};
    if (turn_load(db, session_id, &t) != 0) {
        turn_free(&t);
        return fail(err_out, TURN_REVIEW_ERROR, "could not read the turn: %s",
                    sqlite3_errmsg(db));
    }
    int before = 0;
    TurnReviewResult rc = check_turn(&t, &before, err_out);
    if (rc == TURN_REVIEW_NOTHING) rc = mark_drops(&t, reply, err_out);
    if (rc == TURN_REVIEW_NOTHING) rc = mark_trims(db, &t, reply, err_out);
    if (rc != TURN_REVIEW_NOTHING) goto out;
    mark_iterations(&t);

    int removed = 0, trimmed = 0;
    for (int i = 0; i < t.n; i++) removed += t.rows[i].remove;
    for (int k = 0; k < reply->ntrim; k++) trimmed += reply->trim[k].excerpt != NULL;
    if (!removed && !trimmed && !(reply->note && reply->note[0])) goto out;

    int64_t review_id = 0;
    int on_chain = write_chain(db, session_id, &t, reply, model, &review_id);
    if (on_chain < 0 ||
        /* Each insert moved the leaf to itself (entries_leaf_ai); the answer
         * is still the newest entry of the branch. Put back before the
         * record is written, which measures the turn from the leaf. */
        exec_ids(db, "UPDATE sessions SET leaf_id=?2 WHERE id=?1;", session_id,
                 t.leaf_id) != 0 ||
        write_record(db, session_id, &t, reply, review_id, before) != 0) {
        rc = fail(err_out, TURN_REVIEW_ERROR, "write failed: %s", sqlite3_errmsg(db));
        goto out;
    }
    /* The rows that stayed, plus the review entry (a replacement stands in
     * for the result it replaced). */
    if (on_chain != t.n - removed + 1 || !chain_sound(db, session_id, &t, on_chain)) {
        rc = fail(err_out, TURN_REVIEW_ERROR,
                  "BUG: edited turn %lld is not a single chain — nothing applied",
                  (long long)t.turn_id);
        LOG_ERROR_("turn review session=%lld turn=%lld: edited chain failed its check",
                   (long long)session_id, (long long)t.turn_id);
        goto out;
    }
    rc = TURN_REVIEW_APPLIED;
out:
    turn_free(&t);
    return rc;
}

TurnReviewResult turn_review_apply(sqlite3 *db, int64_t session_id,
                                   const char *reply_json,
                                   const char *reviewer_model, char **err_out) {
    if (err_out) *err_out = NULL;
    if (!db || !reply_json)
        return fail(err_out, TURN_REVIEW_ERROR, "no database or no reply");
    if (!sqlite3_get_autocommit(db))
        return fail(err_out, TURN_REVIEW_ERROR,
                    "BUG: turn_review_apply called inside a transaction");

    Reply reply = {0};
    TurnReviewResult rc = reply_parse(db, reply_json, &reply, err_out);
    if (rc == TURN_REVIEW_NOTHING) rc = note_check(db, &reply, err_out);
    /* {} and its spellings ("drop":[], an empty note) ask for nothing. */
    if (rc != TURN_REVIEW_NOTHING ||
        (!reply.ndrop && !reply.ntrim && !(reply.note && reply.note[0]))) {
        reply_free(&reply);
        return rc;
    }

    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        reply_free(&reply);
        return fail(err_out, TURN_REVIEW_ERROR, "BEGIN failed: %s", sqlite3_errmsg(db));
    }
    rc = apply_locked(db, session_id, &reply, reviewer_model, err_out);
    if (rc != TURN_REVIEW_APPLIED ||
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        if (rc == TURN_REVIEW_APPLIED)
            rc = fail(err_out, TURN_REVIEW_ERROR, "COMMIT failed: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    reply_free(&reply);
    return rc;
}
