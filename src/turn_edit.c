#define _POSIX_C_SOURCE 200809L
#include "turn_edit.h"
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

/* The rows that make an iteration what the model said in one response. */
#define SQL_ITERATION_TYPES "('reasoning','assistant_message','tool_call')"

/* ?1 = session id. No row when the leaf is not an assistant message. The
 * last iteration is the answer, reported apart as `final`. An iteration's
 * entry_id is its first row on the branch — what a note's "before" names. */
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
    "  'tokens',json_object("
    "    'turn',(SELECT COALESCE(SUM(token_estimate),0) FROM part),"
    "    'tools',(SELECT COALESCE(SUM(token_estimate),0) FROM part"
    "              WHERE type IN ('tool_call','tool_result'))),"
    "  'user_messages',(SELECT json_group_array("
    "      json_object('entry_id',u.id,'text',COALESCE(u.content,'')) ORDER BY u.lvl DESC)"
    "    FROM part u WHERE u.type='user_message'),"
    "  'notes',(SELECT json_group_array(json_object("
    "      'entry_id',n.id,'text',COALESCE(n.content,''),"
    "      'before_iteration',(SELECT a.iteration_id FROM part a"
    "         WHERE a.lvl < n.lvl AND a.type IN " SQL_ITERATION_TYPES
    "         ORDER BY a.lvl DESC LIMIT 1)) ORDER BY n.lvl DESC)"
    "    FROM part n WHERE n.type='system'),"
    "  'iterations',(SELECT json_group_array(json(i.obj) ORDER BY i.lvl DESC) FROM ("
    "    SELECT MAX(a.lvl) AS lvl, json_object("
    "      'iteration_id',a.iteration_id,"
    "      'entry_id',(SELECT p.id FROM part p WHERE p.iteration_id=a.iteration_id"
    "         AND p.type IN " SQL_ITERATION_TYPES " ORDER BY p.lvl DESC LIMIT 1),"
    "      'text',COALESCE(MAX(CASE WHEN a.type='assistant_message' THEN a.content END),''),"
    "      'calls',json_group_array(json(json_object("
    "        'call_id',a.tool_call_id,"
    "        'entry_id',a.id,"
    "        'tool',a.tool_name,"
    "        'args',CASE WHEN json_valid(a.content) THEN json(a.content) ELSE a.content END,"
    "        'protected',json(CASE WHEN " SQL_CALL_DROPPABLE("a")
    "            AND " SQL_CALL_UNFINISHED("a") " IS NULL THEN 'false' ELSE 'true' END),"
    "        'is_error',json(CASE WHEN COALESCE((SELECT r.is_error" SQL_RESULT_OF_A "),0)"
    "            THEN 'true' ELSE 'false' END),"
    "        'result',json((SELECT json_object("
    "            'entry_id',r.id,"
    "            'text',COALESCE(r.content,''),"
    "            'tokens',r.token_estimate)" SQL_RESULT_OF_A "))"
    "        )) ORDER BY a.lvl DESC) FILTER (WHERE a.type='tool_call')) AS obj"
    "    FROM part a WHERE a.type IN ('assistant_message','tool_call') AND a.lvl > 0"
    "    GROUP BY a.iteration_id) i),"
    "  'final',json_object('entry_id',f.id,'iteration_id',f.iteration_id,"
    "                      'text',COALESCE(f.content,'')))"
    " FROM part f WHERE f.lvl=0 AND f.type='assistant_message';";

char *turn_edit_input(sqlite3 *db, int64_t session_id) {
    if (!db) return NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL_INPUT, -1, &st, NULL) != SQLITE_OK) {
        LOG_ERROR_("turn edit input: %s", sqlite3_errmsg(db));
        return NULL;
    }
    sqlite3_bind_int64(st, 1, session_id);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

/* ── The reply ─────────────────────────────────────────────────────*/

typedef struct {
    char op;            /* 'd'rop, 'r'eplace, 'n'ote */
    char *call_id;      /* drop, replace */
    char *text;         /* replace, note */
    int has_before;     /* note */
    int64_t before;
    int row;            /* replace: the result it replaces; note: the row it precedes */
    int64_t new_id;     /* replace, note: the entry written */
} Edit;

typedef struct {
    const char *json;   /* the reply as received */
    Edit *e;
    int n;
} Reply;

static void reply_free(Reply *r) {
    for (int i = 0; i < r->n; i++) {
        free(r->e[i].call_id);
        free(r->e[i].text);
    }
    free(r->e);
    memset(r, 0, sizeof(*r));
}

static TurnEditResult fail(char **err_out, TurnEditResult rc, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static TurnEditResult fail(char **err_out, TurnEditResult rc, const char *fmt, ...) {
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

#define SQL_OP "json_extract(e.value,'$.op')"

/* First thing wrong with the reply's shape, or NULL. ?1 = the reply, already
 * known to be a JSON object; ?2 = the record cap. Every arm that reads into
 * a value is guarded by the arm that checks its type: COALESCE stops at the
 * first hit, and CASE keeps json functions off values that are not JSON. */
static const char SQL_REPLY_SHAPE[] =
    "SELECT COALESCE("
    " (SELECT 'unknown key \"' || key || '\"' FROM json_each(?1)"
    "   WHERE key NOT IN ('edits','record') LIMIT 1),"
    " (SELECT 'key \"' || key || '\" appears twice' FROM json_each(?1)"
    "   GROUP BY key HAVING COUNT(*) > 1 LIMIT 1),"
    " CASE WHEN json_type(?1,'$.edits') <> 'array' THEN '\"edits\" must be an array' END,"
    " CASE WHEN json_type(?1,'$.record') <> 'object'"
    "      THEN '\"record\" must be an object'"
    "      WHEN length(CAST(json_extract(?1,'$.record') AS BLOB)) > ?2"
    "      THEN '\"record\" is larger than ' || ?2 || ' bytes' END,"
    " (SELECT 'edits[' || key || '] ' || why FROM ("
    "   SELECT e.key, CASE"
    "     WHEN e.type <> 'object' THEN 'must be an object'"
    "     WHEN json_type(e.value,'$.op') IS NOT 'text'"
    "       OR " SQL_OP " NOT IN ('drop','replace','note')"
    "       THEN '\"op\" must be \"drop\", \"replace\" or \"note\"'"
    "     WHEN (SELECT COUNT(*) <> COUNT(DISTINCT k.key) FROM json_each(e.value) k)"
    "       THEN 'names a field twice'"
    "     WHEN EXISTS (SELECT 1 FROM json_each(e.value) k WHERE k.key <> 'op'"
    "        AND NOT (k.key='call_id' AND " SQL_OP " <> 'note')"
    "        AND NOT (k.key='text' AND " SQL_OP " <> 'drop')"
    "        AND NOT (k.key='before' AND " SQL_OP " = 'note'))"
    "       THEN 'has a field its op does not take'"
    "     WHEN " SQL_OP " <> 'note' AND json_type(e.value,'$.call_id') IS NOT 'text'"
    "       THEN 'needs \"call_id\", a string'"
    "     WHEN " SQL_OP " <> 'drop' AND (json_type(e.value,'$.text') IS NOT 'text'"
    "        OR json_extract(e.value,'$.text') = '')"
    "       THEN 'needs \"text\", a non-empty string'"
    "     WHEN json_type(e.value,'$.before') <> 'integer'"
    "       THEN '\"before\" must be an entry id'"
    "     END AS why FROM json_each(?1,'$.edits') e)"
    "   WHERE why IS NOT NULL ORDER BY key LIMIT 1));";

/* One text answer about the reply; *ok says whether the question ran. */
static char *reply_text(sqlite3 *db, const char *sql, const char *reply, int *ok) {
    sqlite3_stmt *st;
    *ok = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, reply, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, TURN_EDIT_RECORD_MAX_BYTES);
    char *out = NULL;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    *ok = rc == SQLITE_ROW;
    sqlite3_finalize(st);
    return out;
}

static TurnEditResult reply_check(sqlite3 *db, const char *reply, char **err_out) {
    int ok;
    char *kind = reply_text(db,
        "SELECT CASE WHEN json_valid(?1) THEN json_type(?1) END;", reply, &ok);
    int is_object = kind && strcmp(kind, "object") == 0;
    free(kind);
    if (ok && !is_object)
        return fail(err_out, TURN_EDIT_REJECTED, "reply is not a JSON object");
    char *bad = ok ? reply_text(db, SQL_REPLY_SHAPE, reply, &ok) : NULL;
    if (!ok) return fail(err_out, TURN_EDIT_ERROR, "reply check failed: %s",
                         sqlite3_errmsg(db));
    if (!bad) return TURN_EDIT_NOTHING;
    fail(err_out, TURN_EDIT_REJECTED, "%s", bad);
    free(bad);
    return TURN_EDIT_REJECTED;
}

/* The edits into C, in the order given. Reads no table: only JSON functions
 * over a reply whose shape reply_check has passed. */
static TurnEditResult reply_load(sqlite3 *db, Reply *out, char **err_out) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_array_length(?1,'$.edits'), json_extract(value,'$.op'),"
            "  json_extract(value,'$.call_id'), json_extract(value,'$.text'),"
            "  json_extract(value,'$.before')"
            " FROM json_each(?1,'$.edits') ORDER BY key;", -1, &st, NULL) != SQLITE_OK)
        return fail(err_out, TURN_EDIT_ERROR, "reply read failed");
    sqlite3_bind_text(st, 1, out->json, -1, SQLITE_STATIC);
    int oom = 0;
    while (!oom && sqlite3_step(st) == SQLITE_ROW) {
        if (!out->e) out->e = calloc((size_t)sqlite3_column_int(st, 0), sizeof(Edit));
        if (!out->e) { oom = 1; break; }
        Edit *e = &out->e[out->n++];
        const char *cid = (const char *)sqlite3_column_text(st, 2);
        const char *text = (const char *)sqlite3_column_text(st, 3);
        e->op = (char)sqlite3_column_text(st, 1)[0];
        e->call_id = cid ? strdup(cid) : NULL;
        e->text = text ? strdup(text) : NULL;
        e->has_before = sqlite3_column_type(st, 4) != SQLITE_NULL;
        e->before = sqlite3_column_int64(st, 4);
        e->row = -1;
        oom = (cid && !e->call_id) || (text && !e->text);
    }
    if (sqlite3_finalize(st) != SQLITE_OK || oom)
        return fail(err_out, TURN_EDIT_ERROR, "reply read failed");
    return TURN_EDIT_NOTHING;
}

/* Text a script supplies enters every later request, so it passes what any
 * other text entering context passes (inbox_insert_scanned's chain):
 * invisible characters stripped, then stored secret values and scanner rules
 * looked for. Here a hit refuses the reply rather than masking the text — a
 * script that copied a credential out of a tool result got something wrong,
 * and silently storing something other than what it sent would hide that. */
static TurnEditResult text_check(sqlite3 *db, Reply *r, char **err_out) {
    size_t snap_n = 0;
    ShellSecret *snap = secrets_snapshot(db, NULL, 0, &snap_n);
    TurnEditResult rc = TURN_EDIT_NOTHING;
    for (int k = 0; k < r->n && rc == TURN_EDIT_NOTHING; k++) {
        Edit *e = &r->e[k];
        if (!e->text) continue;
        char *clean = unicode_strip_invisible(e->text, strlen(e->text), NULL);
        if (!clean) { rc = fail(err_out, TURN_EDIT_ERROR, "out of memory"); break; }
        free(e->text);
        e->text = clean;
        char *masked = tool_result_postprocess(clean, snap, snap_n);
        if (masked)        /* non-NULL only when something was masked or redacted */
            rc = fail(err_out, TURN_EDIT_REJECTED, "edits[%d]: text contains a secret", k);
        else if (!clean[0])
            rc = fail(err_out, TURN_EDIT_REJECTED,
                      "edits[%d]: text is empty once invisible characters are removed", k);
        free(masked);
    }
    secrets_snapshot_free(snap, snap_n);
    return rc;
}

/* ── The turn, in memory for one apply ─────────────────────────────*/

typedef enum { ROW_OTHER, ROW_ASSISTANT, ROW_REASONING, ROW_CALL, ROW_RESULT } RowKind;

typedef struct {
    int64_t id, parent_id, iteration_id;
    RowKind kind;
    char *call_id;
    int tokens;
    int droppable;          /* calls */
    char unfinished[16];    /* calls: the tool_calls status when not 'done' */
    int claimed;            /* calls: an edit names it */
    int remove;             /* leaves the branch */
    int edit;               /* results: index of the edit replacing it, or -1 */
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
        r->edit = -1;
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

/* The turn is finished: its leaf is the answer and no call is waiting to
 * run. A turn edited before may be edited again — each hook sees the turn as
 * the one before it left it. */
static TurnEditResult check_turn(const Turn *t, char **err_out) {
    if (t->n == 0 || t->rows[t->n - 1].kind != ROW_ASSISTANT)
        return fail(err_out, TURN_EDIT_REJECTED,
                    "the session's newest entry is not an assistant message —"
                    " no finished turn to edit");
    for (int i = 0; i < t->n; i++) {
        const Row *r = &t->rows[i];
        if (strcmp(r->unfinished, "pending") == 0 || strcmp(r->unfinished, "running") == 0)
            return fail(err_out, TURN_EDIT_REJECTED,
                        "call \"%.80s\" is still %s — the turn is not finished",
                        r->call_id ? r->call_id : "?", r->unfinished);
    }
    return TURN_EDIT_NOTHING;
}

/* The one row of `kind` carrying this call id: its index, -1 none, -2 more
 * than one. Results pair by call id alone (see SQL_RESULT_OF_A). */
static int find_row(const Turn *t, RowKind kind, const char *call_id) {
    int found = -1;
    for (int i = 0; i < t->n; i++) {
        if (t->rows[i].kind != kind || !t->rows[i].call_id ||
            strcmp(t->rows[i].call_id, call_id) != 0)
            continue;
        if (found >= 0) return -2;
        found = i;
    }
    return found;
}

/* A drop or a replace: the call must be this turn's, named once, and
 * answered for good. A protected call keeps its place on the branch but its
 * result's text may be replaced. */
static TurnEditResult mark_call_edit(Turn *t, Edit *e, int k, char **err_out) {
    const char *verb = e->op == 'd' ? "drop" : "replace", *cid = e->call_id;
    int ci = find_row(t, ROW_CALL, cid);
    if (ci < 0)
        return fail(err_out, TURN_EDIT_REJECTED, ci == -1
                    ? "%s: \"%.80s\" is not a tool call of this turn"
                    : "%s: call id \"%.80s\" names more than one call in this turn",
                    verb, cid);
    Row *c = &t->rows[ci];
    if (c->unfinished[0])
        return fail(err_out, TURN_EDIT_REJECTED, "%s: call \"%.80s\" is not finished (%s)",
                    verb, cid, c->unfinished);
    if (c->claimed)
        return fail(err_out, TURN_EDIT_REJECTED,
                    "%s: call \"%.80s\" is named by two edits", verb, cid);
    c->claimed = 1;

    if (e->op == 'r') {
        e->row = find_row(t, ROW_RESULT, cid);
        if (e->row < 0)
            return fail(err_out, TURN_EDIT_REJECTED, "replace: call \"%.80s\" has %s", cid,
                        e->row == -2 ? "more than one result" : "no result to replace");
        t->rows[e->row].edit = k;
        return TURN_EDIT_NOTHING;
    }
    if (!c->droppable)
        return fail(err_out, TURN_EDIT_REJECTED,
                    "drop: call \"%.80s\" is protected and cannot be dropped", cid);
    /* Iterations are told apart by id; without one there is no saying which
     * assistant message a call belongs to. */
    if (c->iteration_id <= 0)
        return fail(err_out, TURN_EDIT_REJECTED,
                    "drop: call \"%.80s\" has no iteration id", cid);
    for (int i = 0; i < t->n; i++)
        if (i == ci || (t->rows[i].kind == ROW_RESULT && t->rows[i].call_id &&
                        strcmp(t->rows[i].call_id, cid) == 0))
            t->rows[i].remove = 1;
    return TURN_EDIT_NOTHING;
}

/* Rows of an iteration still on the branch: its calls, or its calls and
 * assistant message. */
static int iteration_left(const Turn *t, int64_t iteration_id, int calls_only) {
    int left = 0;
    for (int i = 0; i < t->n; i++) {
        const Row *r = &t->rows[i];
        if (r->iteration_id == iteration_id && !r->remove &&
            (r->kind == ROW_CALL || (!calls_only && r->kind == ROW_ASSISTANT)))
            left++;
    }
    return left;
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
        int left = iteration_left(t, c->iteration_id, 1);
        for (int j = 0; j < t->n - 1; j++) {
            Row *r = &t->rows[j];
            if (r->iteration_id != c->iteration_id) continue;
            if (r->kind == ROW_REASONING ||
                (!left && (r->kind == ROW_ASSISTANT || r->kind == ROW_RESULT)))
                r->remove = 1;
        }
    }
}

/* Is row i the first row of its iteration? Only there may a note go: any
 * other place would part an assistant message from its calls, or a call from
 * its results. */
static int opens_iteration(const Turn *t, int i) {
    const Row *r = &t->rows[i];
    if (r->kind != ROW_ASSISTANT && r->kind != ROW_REASONING && r->kind != ROW_CALL)
        return 0;
    for (int j = 0; j < i; j++)
        if (t->rows[j].iteration_id == r->iteration_id &&
            (t->rows[j].kind == ROW_ASSISTANT || t->rows[j].kind == ROW_REASONING ||
             t->rows[j].kind == ROW_CALL))
            return 0;
    return 1;
}

/* Where each note goes: before the iteration it names, or before the
 * answer's — reasoning entry included, so that stays next to the answer.
 * Runs after the drops are marked: an iteration this reply empties is no
 * place to stand. */
static TurnEditResult place_notes(const Turn *t, Reply *reply, char **err_out) {
    int answer = t->n - 1;
    if (answer > 0 && t->rows[answer - 1].kind == ROW_REASONING &&
        t->rows[answer - 1].iteration_id == t->rows[answer].iteration_id)
        answer--;
    for (int k = 0; k < reply->n; k++) {
        Edit *e = &reply->e[k];
        if (e->op != 'n') continue;
        e->row = e->has_before ? -1 : answer;
        for (int i = 0; i < t->n && e->row < 0; i++)
            if (t->rows[i].id == e->before && (i == answer || opens_iteration(t, i)))
                e->row = i;
        if (e->row < 0)
            return fail(err_out, TURN_EDIT_REJECTED,
                        "note: \"before\" %lld is not the first entry of an iteration"
                        " of this turn", (long long)e->before);
        if (e->row != answer && !iteration_left(t, t->rows[e->row].iteration_id, 0))
            return fail(err_out, TURN_EDIT_REJECTED,
                        "note: \"before\" %lld names an iteration this reply drops",
                        (long long)e->before);
    }
    return TURN_EDIT_NOTHING;
}

/* ── Writing ───────────────────────────────────────────────────────*/

static int exec_ids(sqlite3 *db, const char *sql, int64_t a, int64_t b, int64_t c) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, a);
    sqlite3_bind_int64(st, 2, b);
    sqlite3_bind_int64(st, 3, c);   /* out of range, and ignored, where unused */
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* One new entry under `parent` carrying a script's text. ?1 = the entry it
 * copies from or the session, ?2 parent, ?3 text, ?4 tokens, ?5 bytes,
 * ?6 turn id, ?7 label. turn_id is always given, so entries_turn_ai leaves
 * the row alone. */
static int64_t insert_text(sqlite3 *db, const char *sql, int64_t ref, int64_t parent,
                           const char *text, int64_t turn_id, const char *label) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    int len = (int)strlen(text);
    sqlite3_bind_int64(st, 1, ref);
    sqlite3_bind_int64(st, 2, parent);
    sqlite3_bind_text(st, 3, text, len, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, TOKEN_ESTIMATE(len));
    sqlite3_bind_int(st, 5, len);
    sqlite3_bind_int64(st, 6, turn_id);
    sqlite3_bind_text(st, 7, label, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE && changed == 1 ? sqlite3_last_insert_rowid(db) : -1;
}

/* A tool_result standing in for entry ?1: everything that says what it
 * answers and where its content came from is carried over — fence_id and
 * network_hosts keep the untrusted-content wrap on replaced network output. */
static const char SQL_INSERT_REPLACEMENT[] =
    "INSERT INTO entries (parent_id, session_id, turn_id, iteration_id, type,"
    "  part_index, role, content, tool_call_id, tool_name, is_error,"
    "  network_hosts, fence_id, token_estimate, content_bytes)"
    " SELECT ?2, session_id, turn_id, iteration_id, type, part_index, role,"
    "  ?3, tool_call_id, tool_name, is_error, network_hosts, fence_id, ?4, ?5"
    " FROM entries WHERE id=?1;";

/* A note is an ordinary system entry, shaped like every other one written
 * mid-session (advance.c's streak_note): type 'system', role 0, iteration 0,
 * provenance in data.source. Role 0 is what keeps it out of every reader
 * that picks entries by role; the transcript sends it by its type, and reads
 * a 'hook:<extension>' source to say whose words it is (llm_payload.c's
 * SQL_SYSTEM_NOTE_PREFIX). ?1 = session id. */
static const char SQL_INSERT_NOTE[] =
    "INSERT INTO entries (parent_id, session_id, turn_id, iteration_id, type, role,"
    "  content, token_estimate, content_bytes, data)"
    " VALUES (?2, ?1, ?6, 0, 'system', 0, ?3, ?4, ?5, json_object('source',?7));";

/* Rebuild the turn's chain front to back: every row that stays hangs off the
 * row that now precedes it. A re-pointed row remembers the parent it was
 * written under — the first one only, so the chain as written can always be
 * walked back (a row that leaves the branch is not touched and still points
 * at its own original parent). Returns the number of rows on the new chain,
 * or -1. */
static int write_chain(sqlite3 *db, int64_t session_id, Turn *t, Reply *reply,
                       const char *label) {
    int64_t prev = t->before_id;
    int on_chain = 0;
    for (int i = 0; i < t->n; i++) {
        Row *r = &t->rows[i];
        for (int k = 0; k < reply->n; k++) {
            Edit *e = &reply->e[k];
            if (e->op != 'n' || e->row != i) continue;
            e->new_id = insert_text(db, SQL_INSERT_NOTE, session_id, prev, e->text,
                                    t->turn_id, label);
            if (e->new_id <= 0) return -1;
            prev = e->new_id;
            on_chain++;
        }
        if (r->remove) continue;
        if (r->edit >= 0) {
            Edit *e = &reply->e[r->edit];
            e->new_id = insert_text(db, SQL_INSERT_REPLACEMENT, r->id, prev, e->text,
                                    t->turn_id, NULL);
            if (e->new_id <= 0) return -1;
            prev = e->new_id;
        } else {
            if (r->parent_id != prev &&
                exec_ids(db, "UPDATE entries SET"
                             " original_parent_id=COALESCE(original_parent_id, parent_id),"
                             " parent_id=?2 WHERE id=?1;", r->id, prev, 0) != 0)
                return -1;
            prev = r->id;
        }
        on_chain++;
    }
    return on_chain;
}

/* The record's frame, appended to $.turn_edits of the answer's data (the
 * leaf — put back before this runs, and what 'after' is measured from).
 * Never sent to the model. ?1 = session id, ?2 label, ?3 tokens before,
 * ?4 the reply, whose "record" rides along untouched when it has one. */
static const char SQL_RECORD[] =
    "WITH RECURSIVE " SQL_TURN
    " UPDATE entries SET data=json_insert("
    "   CASE WHEN json_type(data,'$.turn_edits') IS 'array' THEN data"
    "        ELSE json_set(COALESCE(data,'{}'),'$.turn_edits',json('[]')) END,"
    "   '$.turn_edits[#]', json((SELECT CASE WHEN rec IS NULL THEN obj"
    "       ELSE json_insert(obj,'$.record',json(rec)) END FROM (SELECT"
    "     json_extract(?4,'$.record') AS rec,"
    "     json_object('by',?2,'at',unixepoch(),'left_branch',json('[]'),"
    "       'replaced',json('[]'),'notes',json('[]'),"
    "       'tokens',json_object('before',?3,'after',"
    "         (SELECT COALESCE(SUM(e.token_estimate),0)"
    "            FROM turn t JOIN entries e ON e.id=t.id))) AS obj))))"
    " WHERE id=(SELECT leaf_id FROM sessions WHERE id=?1);";

#define SQL_RECORD_ADD(list, value) \
    "UPDATE entries SET data=json_insert(data,'$.turn_edits[#-1]." list "[#]'," \
    value ") WHERE id=?1;"

/* What the runtime vouches for: every entry that left the branch (in chain
 * order, replaced originals among them), which entry replaced which, the
 * notes written, and the turn's size before and after. */
static int write_record(sqlite3 *db, int64_t session_id, const Turn *t,
                        const Reply *reply, const char *label) {
    int before = 0;
    for (int i = 0; i < t->n; i++) before += t->rows[i].tokens;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL_RECORD, -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_text(st, 2, label, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, before);
    sqlite3_bind_text(st, 4, reply->json, -1, SQLITE_STATIC);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1) return -1;

    for (int i = 0; i < t->n; i++)
        if ((t->rows[i].remove || t->rows[i].edit >= 0) &&
            exec_ids(db, SQL_RECORD_ADD("left_branch", "?2"), t->leaf_id,
                     t->rows[i].id, 0) != 0)
            return -1;
    for (int k = 0; k < reply->n; k++) {
        const Edit *e = &reply->e[k];
        if (e->op == 'r' &&
            exec_ids(db, SQL_RECORD_ADD("replaced",
                         "json_object('original',?2,'replacement',?3)"),
                     t->leaf_id, t->rows[e->row].id, e->new_id) != 0)
            return -1;
        if (e->op == 'n' &&
            exec_ids(db, SQL_RECORD_ADD("notes", "?2"), t->leaf_id, e->new_id, 0) != 0)
            return -1;
    }
    return 0;
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
static TurnEditResult apply_locked(sqlite3 *db, int64_t session_id, Reply *reply,
                                   const char *label, char **err_out) {
    Turn t = {0};
    if (turn_load(db, session_id, &t) != 0) {
        turn_free(&t);
        return fail(err_out, TURN_EDIT_ERROR, "could not read the turn: %s",
                    sqlite3_errmsg(db));
    }
    TurnEditResult rc = check_turn(&t, err_out);
    for (int k = 0; k < reply->n && rc == TURN_EDIT_NOTHING; k++)
        if (reply->e[k].op != 'n') rc = mark_call_edit(&t, &reply->e[k], k, err_out);
    if (rc == TURN_EDIT_NOTHING) {
        mark_iterations(&t);
        rc = place_notes(&t, reply, err_out);
    }
    if (rc != TURN_EDIT_NOTHING) goto out;

    int on_chain = write_chain(db, session_id, &t, reply, label);
    if (on_chain < 0 ||
        /* Each insert moved the leaf to itself (entries_leaf_ai); the answer
         * is still the newest entry of the branch. */
        exec_ids(db, "UPDATE sessions SET leaf_id=?2 WHERE id=?1;", session_id,
                 t.leaf_id, 0) != 0 ||
        write_record(db, session_id, &t, reply, label) != 0) {
        rc = fail(err_out, TURN_EDIT_ERROR, "write failed: %s", sqlite3_errmsg(db));
        goto out;
    }
    if (!chain_sound(db, session_id, &t, on_chain)) {
        rc = fail(err_out, TURN_EDIT_ERROR,
                  "BUG: edited turn %lld is not a single chain — nothing applied",
                  (long long)t.turn_id);
        LOG_ERROR_("turn edit session=%lld turn=%lld: edited chain failed its check",
                   (long long)session_id, (long long)t.turn_id);
        goto out;
    }
    rc = TURN_EDIT_APPLIED;
out:
    turn_free(&t);
    return rc;
}

TurnEditResult turn_edit_apply(sqlite3 *db, int64_t session_id, const char *reply_json,
                               const char *source_label, char **err_out) {
    if (err_out) *err_out = NULL;
    if (!db || !reply_json)
        return fail(err_out, TURN_EDIT_ERROR, "no database or no reply");
    if (!sqlite3_get_autocommit(db))
        return fail(err_out, TURN_EDIT_ERROR,
                    "BUG: turn_edit_apply called inside a transaction");

    Reply reply = { .json = reply_json };
    TurnEditResult rc = reply_check(db, reply_json, err_out);
    if (rc == TURN_EDIT_NOTHING) rc = reply_load(db, &reply, err_out);
    if (rc == TURN_EDIT_NOTHING) rc = text_check(db, &reply, err_out);
    /* No edits asks for nothing, record or not: a record describes edits. */
    if (rc != TURN_EDIT_NOTHING || reply.n == 0) {
        reply_free(&reply);
        return rc;
    }

    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) {
        reply_free(&reply);
        return fail(err_out, TURN_EDIT_ERROR, "BEGIN failed: %s", sqlite3_errmsg(db));
    }
    rc = apply_locked(db, session_id, &reply, source_label, err_out);
    if (rc != TURN_EDIT_APPLIED ||
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        if (rc == TURN_EDIT_APPLIED)
            rc = fail(err_out, TURN_EDIT_ERROR, "COMMIT failed: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    }
    reply_free(&reply);
    return rc;
}
