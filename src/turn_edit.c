#define _POSIX_C_SOURCE 200809L
#include "turn_edit.h"
#include "buf.h"
#include "config.h"
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

/* ── Walks ─────────────────────────────────────────────────────────
 *
 * Every walk here starts at the session's leaf and goes towards the root.
 * One test says where a turn ends — two adjacent rows are the same turn when
 * their turn_id is — and the input's units (SQL_INPUT) and the edits'
 * branch (SQL_BRANCH_ROWS, branch_load) both read it. ?1 = session id. */
#define SQL_LEAF_ROW "id=(SELECT leaf_id FROM sessions WHERE id=?1) AND session_id=?1"
#define SQL_SAME_TURN(a, b) "(" a ".turn_id=" b ".turn_id)"

/* May a call be dropped? Both read off the tool_call entry aliased C.
 *
 * Droppable is a fact about the tool (tools.droppable — the built-in recipe
 * flag, or an extension manifest's word; a tool with no row is protected),
 * with one exception that is a fact about the call: save_secret turns any
 * retrieving tool into one that stored a credential, and that is an effect.
 *
 * Finished: tool_calls is workflow state, and anything but 'done' means
 * something is still going to answer this call (a background job). A call
 * with no row at all was answered at ingest (unparseable arguments).
 *
 * A call whose arguments were rewritten is a new row standing in for the one
 * the model wrote (data.replaces names that first original): its workflow row
 * and the arguments it was made with are the original's. Both tests below
 * read the call as the model made it, so a rewrite cannot turn a call that
 * stored a credential into one that may be dropped. */
#define SQL_CALL_ROOT(C) \
    "COALESCE(CASE WHEN json_valid(" C ".data) THEN json_extract(" C ".data,'$.replaces') END," \
    C ".id)"
#define SQL_SAVES_SECRET(X) \
    "COALESCE(CASE WHEN json_valid(" X ") THEN json_extract(" X ",'$.save_secret') END,'')<>''"
#define SQL_CALL_DROPPABLE(C) \
    "(COALESCE((SELECT tl.droppable FROM tools tl WHERE tl.name=" C ".tool_name),0)=1" \
    " AND NOT " SQL_SAVES_SECRET(C ".content") \
    " AND NOT COALESCE((SELECT " SQL_SAVES_SECRET("o.content") " FROM entries o" \
    "   WHERE o.id=" SQL_CALL_ROOT(C) "),0))"
#define SQL_CALL_UNFINISHED(C) \
    "(SELECT tc.status FROM tool_calls tc" \
    "  WHERE tc.entry_id=" SQL_CALL_ROOT(C) " AND tc.status<>'done' LIMIT 1)"

/* The rows that make an iteration what the model said in one response. */
#define SQL_ITERATION_TYPES "('reasoning','assistant_message','tool_call')"

/* The rows the input writes out text for. A reasoning row's text is the
 * readable summary the model was asked for (agent_models.reasoning_display),
 * sent as its iteration's `reasoning`; the signature and the replay blob
 * (reasoning_meta) never are — a script never sees thinking itself. */
#define SQL_SENT_TYPES \
    "('user_message','system','assistant_message','tool_call','tool_result','compaction'," \
    "'reasoning')"

/* ── The session as a hook's input ─────────────────────────────────
 *
 * One statement, one walk of the active branch, exact about max_bytes.
 *
 * `branch` walks leaf (lvl 0) to root, numbering units as it goes: `run` 0 is
 * the newest turn (the rows from the leaf back while the turn_id stays the
 * leaf's) and every higher run is an earlier turn, or a summary standing
 * between turns, which is a unit of its own (SQL_SEP: kept or left out apart
 * from the turns around it). `t` counts the earlier turns walked through; the
 * walk stops at the first unit past the ?2 asked for, so with no history
 * asked for it reads the newest turn's rows and no others.
 *
 * Which earlier units are sent is decided on the text as written out, not on
 * an estimate. First a cheap pass on integers: `priced` takes each row's
 * stored length for what will be written out of it (0 for a row that is not:
 * an earlier result without its text, an assistant entry that stopped on an
 * error, which the next request never sends either — context_plan — and that
 * response's reasoning; a reasoning row is its kept summary, empty when none
 * was), and `pick` keeps the earlier units, newest first, whose
 * stored text alone could still fit beside the newest turn. Text is read for
 * those and nothing older, each unit is serialised once, and `fit` keeps
 * them, newest first, while the bytes written out still fit — so the oldest
 * turns are the ones left out, whole, and a summary goes only after every
 * turn older than it. The pieces are joined as text: each is a JSON object
 * json_object wrote, so dropping a closing brace and an opening one splices
 * them, and what is measured is exactly what is handed over.
 *
 * Every per-turn and per-iteration part is an aggregate over `part` grouped
 * once and joined back (`ums`, `notes`, `calls`, `iters`, ...), not a
 * subquery per turn: the work grows with the rows sent, not with their
 * square. What comes out of a materialised CTE has lost its JSON subtype,
 * hence the json() around each.
 *
 * ?1 session id, ?2 earlier turns wanted (-1 all, 0 none), ?3 earlier
 * results with their text (0/1), ?4 max_bytes, ?5 the event name, ?6 1 when
 * the newest turn must have ended with an answer, ?7 a JSON object sent as
 * "context", or NULL. */
/* A summary standing between turns (it carries the id of the first turn it
 * replaced — mark_span — so never the id of the turn before it or after it)
 * is a unit of its own; a summary of iterations of a turn carries that
 * turn's id and is part of it, read with its notes. */
#define SQL_SEP "(e.type='compaction' AND e.turn_id IS NOT b.turn_id)"
#define SQL_NEW_UNIT "(COALESCE(NOT " SQL_SAME_TURN("e", "b") ", 1) OR b.sep)"
#define SQL_ROW_ERRORED "(e.role=2 AND COALESCE(e.stop_reason,0) IN (4,5))"
/* A reasoning row goes with its response: left out of an earlier turn when
 * the response's message stopped on an error, like the message. */
#define SQL_RESPONSE_ERRORED \
    "(" SQL_ROW_ERRORED " OR (e.type='reasoning' AND EXISTS(SELECT 1 FROM entries m" \
    "  WHERE m.session_id=?1 AND m.iteration_id=e.iteration_id" \
    "    AND m.type='assistant_message' AND COALESCE(m.stop_reason,0) IN (4,5))))"
/* Room kept for what is spliced in after the newest turn is measured:
 * ',"history":[', '],"history_omitted":' and a count. */
#define SQL_SPLICE_ROOM "64"

static const char SQL_INPUT[] =
    "WITH RECURSIVE"
    " branch(id, parent_id, turn_id, type, lvl, run, t, sep) AS ("
    "  SELECT id, parent_id, turn_id, type, 0, 0, 0, 0 FROM entries WHERE " SQL_LEAF_ROW
    "  UNION ALL"
    "  SELECT e.id, e.parent_id, e.turn_id, e.type, b.lvl+1,"
    "         b.run + " SQL_NEW_UNIT ","
    "         b.t + (" SQL_NEW_UNIT " AND NOT " SQL_SEP "), " SQL_SEP
    "    FROM entries e JOIN branch b ON e.id=b.parent_id"
    "   WHERE e.session_id=?1 AND (?2 < 0 OR b.t < ?2 OR NOT " SQL_NEW_UNIT ")),"
    " priced AS MATERIALIZED ("
    "  SELECT b.run, b.lvl, b.id, b.type, b.sep,"
    "         b.run=0 OR NOT " SQL_RESPONSE_ERRORED " AS keep,"
    "         CASE WHEN b.run > 0 AND " SQL_RESPONSE_ERRORED " THEN 0"
    "              WHEN b.type NOT IN " SQL_SENT_TYPES " THEN 0"
    "              WHEN b.run > 0 AND b.type='tool_result' AND NOT ?3 THEN 0"
    "              ELSE COALESCE(e.content_bytes,0) END AS bytes"
    "    FROM branch b JOIN entries e ON e.id=b.id),"
    " units AS MATERIALIZED ("
    "  SELECT run, MAX(sep) AS summary, SUM(bytes) AS bytes"
    "    FROM priced GROUP BY run),"
    " pick AS MATERIALIZED ("
    "  SELECT run FROM (SELECT run, SUM(bytes) OVER (ORDER BY run) AS cum"
    "                     FROM units WHERE run > 0)"
    "   WHERE cum <= ?4 - (SELECT bytes FROM units WHERE run=0)),"
    /* Text, for the rows that are sent, once the newest turn's stored text
     * alone does not already rule it out. */
    " part AS MATERIALIZED ("
    "  SELECT r.run, r.lvl, r.sep, e.id, e.type, e.iteration_id, e.turn_id,"
    "         CASE WHEN r.run > 0 AND e.type='tool_result' AND NOT ?3 THEN NULL"
    "              ELSE e.content END AS content,"
    "         e.tool_call_id, e.tool_name, e.is_error,"
    "         COALESCE(e.tokens, e.token_estimate) AS tokens,"
    "         CASE WHEN e.type='tool_call' THEN " SQL_CALL_DROPPABLE("e") " END AS droppable,"
    "         CASE WHEN e.type='tool_call' THEN " SQL_CALL_UNFINISHED("e") " END AS unfinished"
    "    FROM priced r JOIN entries e ON e.id=r.id"
    "   WHERE r.keep AND (r.run=0 OR r.run IN (SELECT run FROM pick))"
    "     AND (SELECT bytes FROM units WHERE run=0) <= ?4),"
    /* One row per turn: where it starts, and where it ends — its newest row
     * that is not a system note, so a note left after the answer (the streak
     * guard's) does not hide it. A summary between turns is no part of any
     * turn. */
    " turns AS MATERIALIZED ("
    "  SELECT run, turn_id, MAX(lvl) AS first,"
    "         MIN(lvl) FILTER (WHERE type NOT IN ('system','compaction')) AS last"
    "    FROM part WHERE NOT sep GROUP BY run),"
    /* The answer, reported apart as `final`: the turn's end when that is an
     * assistant message. An earlier turn that ended on a cutoff has none. */
    " fin AS MATERIALIZED ("
    "  SELECT k.run, f.id, f.iteration_id, f.content, f.tokens"
    "    FROM turns k JOIN part f ON f.lvl=k.last AND f.type='assistant_message'),"
    " tok AS MATERIALIZED ("
    "  SELECT run, COALESCE(SUM(tokens),0) AS turn,"
    "         COALESCE(SUM(tokens) FILTER (WHERE type IN ('tool_call','tool_result')),0) AS tools"
    "    FROM part WHERE NOT sep GROUP BY run),"
    " ums AS MATERIALIZED ("
    "  SELECT run, json_group_array(json_object('entry_id',id,'text',COALESCE(content,''),"
    "           'tokens',tokens) ORDER BY lvl DESC) AS arr"
    "    FROM part WHERE type='user_message' GROUP BY run),"
    /* A note names the iteration it stands before: the nearest newer row of
     * an iteration, found by a window over the turn, not a search per note.
     * A summary of some of the turn's iterations is listed with the notes,
     * marked "summary". */
    " nx AS MATERIALIZED ("
    "  SELECT id, run, lvl, sep, type, content, tokens,"
    "         MAX(CASE WHEN type IN " SQL_ITERATION_TYPES " THEN lvl END) OVER ("
    "           PARTITION BY run ORDER BY lvl"
    "           ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING) AS next_lvl"
    "    FROM part),"
    " notes AS MATERIALIZED ("
    "  SELECT n.run, json_group_array(json_patch(json_object('entry_id',n.id,"
    "           'text',COALESCE(n.content,''),'tokens',n.tokens,"
    "           'before_iteration',a.iteration_id),"
    "           CASE WHEN n.type='compaction' THEN '{\"summary\":true}' ELSE '{}' END)"
    "           ORDER BY n.lvl DESC) AS arr"
    "    FROM nx n LEFT JOIN part a ON a.run=n.run AND a.lvl=n.next_lvl"
    "   WHERE n.type IN ('system','compaction') AND NOT n.sep GROUP BY n.run),"
    /* The result answering each call, within its turn: paired by call id
     * alone — a result written outside the dispatcher (a sub-agent's answer,
     * an approval notice) is appended under an iteration id of its own, not
     * its call's — and only within the turn: providers reuse call ids across
     * turns. The first one written, should there be two. */
    " res AS MATERIALIZED ("
    "  SELECT run, tool_call_id, id, content, tokens, is_error FROM ("
    "    SELECT run, tool_call_id, id, content, tokens, is_error, ROW_NUMBER() OVER ("
    "             PARTITION BY run, tool_call_id ORDER BY lvl DESC) AS rn"
    "      FROM part WHERE type='tool_result') WHERE rn=1),"
    /* An iteration's entry_id is its first row on the branch — what a note's
     * "before" names. */
    " opener AS MATERIALIZED ("
    "  SELECT run, iteration_id, id FROM ("
    "    SELECT run, iteration_id, id, ROW_NUMBER() OVER ("
    "             PARTITION BY run, iteration_id ORDER BY lvl DESC) AS rn"
    "      FROM part WHERE type IN " SQL_ITERATION_TYPES ") WHERE rn=1),"
    /* Each response's reasoning summary, where one was kept. */
    " rsn AS MATERIALIZED ("
    "  SELECT run, iteration_id, MAX(content) AS content FROM part"
    "   WHERE type='reasoning' AND COALESCE(content,'')<>'' GROUP BY run, iteration_id),"
    /* An earlier turn's results lose their text when ?3 says so (the newest
     * turn's never do). */
    " calls AS MATERIALIZED ("
    "  SELECT a.run, a.iteration_id, json_group_array(json_object("
    "      'call_id',a.tool_call_id,"
    "      'entry_id',a.id,"
    "      'tool',a.tool_name,"
    "      'tokens',a.tokens,"
    "      'args',CASE WHEN json_valid(a.content) THEN json(a.content) ELSE a.content END,"
    "      'protected',json(CASE WHEN a.droppable AND a.unfinished IS NULL"
    "          THEN 'false' ELSE 'true' END),"
    "      'is_error',json(CASE WHEN COALESCE(r.is_error,0) THEN 'true' ELSE 'false' END),"
    "      'result',json(CASE WHEN r.id IS NULL THEN NULL"
    "        WHEN a.run=0 OR ?3 THEN json_object('entry_id',r.id,"
    "          'text',COALESCE(r.content,''),'tokens',r.tokens)"
    "        ELSE json_object('entry_id',r.id,'tokens',r.tokens) END)"
    "      ) ORDER BY a.lvl DESC) AS arr"
    "    FROM part a LEFT JOIN res r ON r.run=a.run AND r.tool_call_id=a.tool_call_id"
    "   WHERE a.type='tool_call' GROUP BY a.run, a.iteration_id),"
    /* Every iteration but the answer's. */
    " iters AS MATERIALIZED ("
    "  SELECT i.run, json_group_array(json(CASE WHEN i.reasoning IS NULL THEN i.obj"
    "      ELSE json_set(i.obj,'$.reasoning',i.reasoning) END) ORDER BY i.lvl DESC) AS arr FROM ("
    "    SELECT a.run, MAX(a.lvl) AS lvl, MAX(rs.content) AS reasoning, json_object("
    "      'iteration_id',a.iteration_id,"
    "      'entry_id',o.id,"
    "      'message_id',MIN(CASE WHEN a.type='assistant_message' THEN a.id END),"
    "      'text',COALESCE(MAX(CASE WHEN a.type='assistant_message' THEN a.content END),''),"
    "      'tokens',COALESCE(SUM(a.tokens) FILTER (WHERE a.type='assistant_message'),0),"
    "      'calls',json(COALESCE(c.arr,'[]'))) AS obj"
    "      FROM part a"
    "      LEFT JOIN fin f ON f.run=a.run"
    "      LEFT JOIN opener o ON o.run=a.run AND o.iteration_id=a.iteration_id"
    "      LEFT JOIN calls c ON c.run=a.run AND c.iteration_id=a.iteration_id"
    "      LEFT JOIN rsn rs ON rs.run=a.run AND rs.iteration_id=a.iteration_id"
    "     WHERE a.type IN ('assistant_message','tool_call') AND a.id IS NOT f.id"
    "     GROUP BY a.run, a.iteration_id) i GROUP BY i.run),"
    /* One turn's fields: the same object is the newest turn at the top level
     * and each earlier turn in `history`. */
    " tobj AS MATERIALIZED ("
    "  SELECT k.run, k.first AS lvl, f.id AS final_id, json_object("
    "    'turn_id',k.turn_id,"
    "    'tokens',json_object('turn',COALESCE(t.turn,0),'tools',COALESCE(t.tools,0)),"
    "    'user_messages',json(COALESCE(u.arr,'[]')),"
    "    'notes',json(COALESCE(n.arr,'[]')),"
    "    'iterations',json(COALESCE(i.arr,'[]')),"
    "    'final',json(CASE WHEN f.id IS NOT NULL THEN json_patch(json_object('entry_id',f.id,"
    "        'iteration_id',f.iteration_id,'text',COALESCE(f.content,''),"
    "        'tokens',f.tokens),"
    "        CASE WHEN fr.content IS NULL THEN '{}'"
    "             ELSE json_object('reasoning',fr.content) END) END)) AS obj"
    "    FROM turns k"
    "    LEFT JOIN tok t ON t.run=k.run LEFT JOIN ums u ON u.run=k.run"
    "    LEFT JOIN notes n ON n.run=k.run LEFT JOIN iters i ON i.run=k.run"
    "    LEFT JOIN fin f ON f.run=k.run"
    "    LEFT JOIN rsn fr ON fr.run=k.run AND fr.iteration_id=f.iteration_id),"
    /* The top level: what the script is told about itself, then the newest
     * turn's fields. No row when the turn has no answer and needs one. */
    " head AS MATERIALIZED ("
    "  SELECT CASE WHEN ?7 IS NULL THEN h ELSE json_set(h,'$.context',json(?7)) END AS obj"
    "    FROM (SELECT json_object("
    "      'event',?5,"
    "      'session_id',?1,"
    "      'agent',(SELECT agent_name FROM sessions WHERE id=?1),"
    "      'input',json_object("
    "        'history',CASE WHEN ?2 < 0 THEN 'all' WHEN ?2 = 0 THEN 'none' ELSE ?2 END,"
    "        'history_results',json(CASE WHEN ?3 THEN 'true' ELSE 'false' END),"
    "        'max_bytes',?4)) AS h)),"
    " top AS MATERIALIZED ("
    "  SELECT substr(h.obj,1,length(h.obj)-1) || ',' || substr(k.obj,2) AS obj"
    "    FROM head h, tobj k WHERE k.run=0 AND (NOT ?6 OR k.final_id IS NOT NULL)),"
    " earlier AS MATERIALIZED ("
    "  SELECT run, lvl, obj FROM tobj WHERE run > 0"
    "  UNION ALL"
    "  SELECT run, lvl, json_object('summary',json_object('entry_id',id,"
    "         'text',COALESCE(content,''),'tokens',tokens))"
    "    FROM part WHERE sep AND run > 0),"
    " fit AS MATERIALIZED ("
    "  SELECT run, obj FROM (SELECT run, obj,"
    "           SUM(octet_length(obj) + 1) OVER (ORDER BY run) AS cum FROM earlier)"
    "   WHERE cum <= ?4 - " SQL_SPLICE_ROOM " - (SELECT octet_length(obj) FROM top))"
    /* The input, or NULL: no answer when one is needed, or the newest turn
     * alone too big (column 1 says which). Column 2 counts earlier units
     * sent. `history` is oldest first. */
    " SELECT (SELECT substr(t.obj,1,length(t.obj)-1)"
    "            || ',\"history\":[' || COALESCE((SELECT group_concat(obj, ','"
    "                                   ORDER BY run DESC) FROM fit),'')"
    "            || '],\"history_omitted\":'"
    "            || (SELECT COUNT(*) FROM units WHERE run > 0 AND NOT summary"
    "                  AND run NOT IN (SELECT run FROM fit)) || '}'"
    "          FROM top t WHERE octet_length(t.obj) + " SQL_SPLICE_ROOM " <= ?4),"
    "        u.bytes > ?4 OR COALESCE((SELECT octet_length(obj) + " SQL_SPLICE_ROOM " > ?4"
    "                                    FROM top), 0),"
    "        (SELECT COUNT(*) FROM fit)"
    "   FROM units u WHERE u.run=0;";

char *turn_edit_input(sqlite3 *db, int64_t session_id, const TurnEditInputSpec *spec,
                      const TurnEditInputFor *what, int *too_big) {
    if (too_big) *too_big = 0;
    if (!db || !spec || !what || !what->event || spec->max_bytes <= 0) return NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL_INPUT, -1, &st, NULL) != SQLITE_OK) {
        LOG_ERROR_("turn edit input: %s", sqlite3_errmsg(db));
        return NULL;
    }
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_int(st, 2, spec->history);
    sqlite3_bind_int(st, 3, spec->history_results ? 1 : 0);
    sqlite3_bind_int64(st, 4, spec->max_bytes);
    sqlite3_bind_text(st, 5, what->event, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 6, what->need_answer ? 1 : 0);
    if (what->context) sqlite3_bind_text(st, 7, what->context, -1, SQLITE_STATIC);
    char *out = NULL;
    int rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
        else if (sqlite3_column_int(st, 1) && too_big) *too_big = 1;
    } else if (rc != SQLITE_DONE) {
        LOG_ERROR_("turn edit input session=%lld: %s", (long long)session_id,
                   sqlite3_errmsg(db));
    }
    sqlite3_finalize(st);
    return out;
}

/* ── The reply ─────────────────────────────────────────────────────
 *
 * Six operations, valid on any turn of the active branch:
 *
 *   drop        a call and its result leave            call_id [turn_id]
 *   replace     new text for a call's result           call_id [turn_id] text
 *   note        a system note before an iteration      text [before]
 *   rewrite     new text for a message, or new         entry_id text
 *               arguments for a call                   | call_id [turn_id] args
 *   drop_turns  a run of whole turns before the        from_turn to_turn
 *               newest leaves
 *   summarise   a run of whole turns before the        text, from_turn to_turn
 *               newest, or of whole iterations of     | from_iteration to_iteration
 *               the newest, is replaced by the text
 *               (with the notes and summaries right
 *               before the iterations: iteration_span)
 *
 * Call ids are scoped to their turn (providers reuse them), so a call is named
 * within turn_id, or within the newest turn when that is absent. */

typedef struct {
    char op;            /* 'd'rop, 'r'eplace, 'n'ote, re'w'rite, drop_'t'urns, 's'ummarise */
    char *call_id;      /* drop, replace, rewrite of a call */
    int has_turn;
    int64_t turn_id;    /* the turn call_id is looked up in */
    char *text;         /* replace, note, rewrite, summarise; a call's rewrite: its arguments */
    int is_args;        /* text is a call's new arguments, as JSON */
    int has_entry;
    int64_t entry_id;   /* rewrite of a message */
    int has_before;
    int64_t before;     /* note */
    int by_turns;       /* drop_turns, summarise: from/to are turn ids, else iteration ids */
    int64_t from, to;
    int row;            /* stand-in: the row it stands in for; insert: the row it goes
                         * before (the branch's length: at the end) */
    int64_t new_turn;   /* insert: the turn it belongs to */
    int64_t new_id;     /* the entry written */
} Edit;

#define IS_INSERT(e) ((e)->op == 'n' || (e)->op == 's')

typedef struct {
    const char *json;   /* the reply as received; NULL for the runtime's own trim */
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
#define SQL_HAS(k) "(json_type(e.value,'$." k "') IS NOT NULL)"
#define SQL_INT_FIELDS \
    "('turn_id','before','entry_id','from_turn','to_turn','from_iteration','to_iteration')"

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
    "       OR " SQL_OP " NOT IN ('drop','replace','note','rewrite','drop_turns','summarise')"
    "       THEN '\"op\" must be one of drop, replace, note, rewrite, drop_turns, summarise'"
    "     WHEN (SELECT COUNT(*) <> COUNT(DISTINCT k.key) FROM json_each(e.value) k)"
    "       THEN 'names a field twice'"
    "     WHEN EXISTS (SELECT 1 FROM json_each(e.value) k WHERE instr(CASE " SQL_OP
    "          WHEN 'drop' THEN ',op,call_id,turn_id,'"
    "          WHEN 'replace' THEN ',op,call_id,turn_id,text,'"
    "          WHEN 'note' THEN ',op,text,before,'"
    "          WHEN 'rewrite' THEN ',op,entry_id,text,call_id,turn_id,args,'"
    "          WHEN 'drop_turns' THEN ',op,from_turn,to_turn,'"
    "          ELSE ',op,text,from_turn,to_turn,from_iteration,to_iteration,' END,"
    "          ',' || k.key || ',') = 0)"
    "       THEN 'has a field its op does not take'"
    "     WHEN EXISTS (SELECT 1 FROM json_each(e.value) k"
    "          WHERE k.key IN " SQL_INT_FIELDS " AND k.type <> 'integer')"
    "       THEN (SELECT '\"' || k.key || '\" must be an entry, turn or iteration id'"
    "               FROM json_each(e.value) k"
    "              WHERE k.key IN " SQL_INT_FIELDS " AND k.type <> 'integer' LIMIT 1)"
    "     WHEN (" SQL_OP " IN ('drop','replace') OR " SQL_HAS("call_id") ")"
    "          AND json_type(e.value,'$.call_id') IS NOT 'text'"
    "       THEN 'needs \"call_id\", a string'"
    "     WHEN " SQL_OP " = 'rewrite' AND " SQL_HAS("entry_id") " = " SQL_HAS("call_id")
    "       THEN 'needs \"entry_id\" (a message) or \"call_id\" (a call), one of them'"
    "     WHEN " SQL_OP " = 'rewrite' AND " SQL_HAS("call_id")
    "          AND (" SQL_HAS("text") " OR json_type(e.value,'$.args') IS NOT 'object')"
    "       THEN 'with \"call_id\" takes \"args\", an object, and no \"text\"'"
    "     WHEN " SQL_OP " = 'rewrite' AND " SQL_HAS("entry_id")
    "          AND (" SQL_HAS("args") " OR " SQL_HAS("turn_id") ")"
    "       THEN 'with \"entry_id\" takes \"text\", not \"args\" or \"turn_id\"'"
    "     WHEN (" SQL_OP " IN ('replace','note','summarise')"
    "          OR (" SQL_OP " = 'rewrite' AND " SQL_HAS("entry_id") "))"
    "          AND (json_type(e.value,'$.text') IS NOT 'text'"
    "               OR json_extract(e.value,'$.text') = '')"
    "       THEN 'needs \"text\", a non-empty string'"
    "     WHEN " SQL_OP " = 'drop_turns' AND NOT (" SQL_HAS("from_turn")
    "          AND " SQL_HAS("to_turn") ")"
    "       THEN 'needs \"from_turn\" and \"to_turn\"'"
    "     WHEN " SQL_OP " = 'summarise' AND NOT ("
    "          (" SQL_HAS("from_turn") " AND " SQL_HAS("to_turn")
    "           AND NOT " SQL_HAS("from_iteration") " AND NOT " SQL_HAS("to_iteration") ")"
    "       OR (" SQL_HAS("from_iteration") " AND " SQL_HAS("to_iteration")
    "           AND NOT " SQL_HAS("from_turn") " AND NOT " SQL_HAS("to_turn") "))"
    "       THEN 'needs \"from_turn\" and \"to_turn\", or \"from_iteration\" and"
    " \"to_iteration\"'"
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
            "  json_extract(value,'$.call_id'),"
            "  CASE WHEN json_type(value,'$.args')='object'"
            "       THEN json(json_extract(value,'$.args')) ELSE json_extract(value,'$.text') END,"
            "  json_type(value,'$.args')='object',"
            "  json_extract(value,'$.turn_id'), json_extract(value,'$.entry_id'),"
            "  json_extract(value,'$.before'),"
            "  COALESCE(json_extract(value,'$.from_turn'), json_extract(value,'$.from_iteration')),"
            "  COALESCE(json_extract(value,'$.to_turn'), json_extract(value,'$.to_iteration')),"
            "  json_type(value,'$.from_turn') IS NOT NULL"
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
        if (strcmp((const char *)sqlite3_column_text(st, 1), "drop_turns") == 0) e->op = 't';
        if (strcmp((const char *)sqlite3_column_text(st, 1), "rewrite") == 0) e->op = 'w';
        e->call_id = cid ? strdup(cid) : NULL;
        e->text = text ? strdup(text) : NULL;
        e->is_args = sqlite3_column_int(st, 4);
        e->has_turn = sqlite3_column_type(st, 5) != SQLITE_NULL;
        e->turn_id = sqlite3_column_int64(st, 5);
        e->has_entry = sqlite3_column_type(st, 6) != SQLITE_NULL;
        e->entry_id = sqlite3_column_int64(st, 6);
        e->has_before = sqlite3_column_type(st, 7) != SQLITE_NULL;
        e->before = sqlite3_column_int64(st, 7);
        e->from = sqlite3_column_int64(st, 8);
        e->to = sqlite3_column_int64(st, 9);
        e->by_turns = sqlite3_column_int(st, 10);
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
 * and silently storing something other than what it sent would hide that.
 * A call's new arguments are JSON and are scanned as they are. */
static TurnEditResult text_check(sqlite3 *db, Reply *r, char **err_out) {
    size_t snap_n = 0;
    ShellSecret *snap = secrets_snapshot(db, NULL, 0, &snap_n);
    TurnEditResult rc = TURN_EDIT_NOTHING;
    for (int k = 0; k < r->n && rc == TURN_EDIT_NOTHING; k++) {
        Edit *e = &r->e[k];
        if (!e->text) continue;
        if (!e->is_args) {
            char *clean = unicode_strip_invisible(e->text, strlen(e->text), NULL);
            if (!clean) { rc = fail(err_out, TURN_EDIT_ERROR, "out of memory"); break; }
            free(e->text);
            e->text = clean;
        }
        char *masked = tool_result_postprocess(e->text, snap, snap_n);
        if (masked)        /* non-NULL only when something was masked or redacted */
            rc = fail(err_out, TURN_EDIT_REJECTED, "edits[%d]: text contains a secret", k);
        else if (!e->text[0])
            rc = fail(err_out, TURN_EDIT_REJECTED,
                      "edits[%d]: text is empty once invisible characters are removed", k);
        free(masked);
    }
    secrets_snapshot_free(snap, snap_n);
    return rc;
}

/* ── The branch, in memory for one edit ───────────────────────────*/

typedef enum {
    ROW_OTHER, ROW_USER, ROW_SYSTEM, ROW_SUMMARY,
    ROW_ASSISTANT, ROW_REASONING, ROW_CALL, ROW_RESULT
} RowKind;

typedef struct {
    int64_t id, parent_id, iteration_id, turn_id;
    RowKind kind;
    char *call_id;
    int tokens;
    int droppable;          /* calls */
    char unfinished[16];    /* calls: the tool_calls status when not 'done' */
    int claimed;            /* 1 + the index of the edit that names it, 0 none */
    int dropped;            /* calls: left by a drop, which decides its iteration's fate */
    int remove;             /* leaves the branch */
    int edit;               /* index of the edit writing its stand-in, or -1 */
} Row;

typedef struct {
    Row *rows;              /* root→leaf */
    int n;
    int64_t leaf_id;
    int64_t before_id;      /* what the first row hangs off (-1: it is the root) */
    int newest;             /* index of the newest turn's first row */
    int answer;             /* index of the newest turn's answer, -1 while it runs */
} Branch;

static void branch_free(Branch *b) {
    for (int i = 0; i < b->n; i++) free(b->rows[i].call_id);
    free(b->rows);
    memset(b, 0, sizeof(*b));
}

static RowKind row_kind(const char *type) {
    static const struct { const char *type; RowKind kind; } MAP[] = {
        {"user_message", ROW_USER}, {"system", ROW_SYSTEM}, {"compaction", ROW_SUMMARY},
        {"assistant_message", ROW_ASSISTANT}, {"reasoning", ROW_REASONING},
        {"tool_call", ROW_CALL}, {"tool_result", ROW_RESULT},
    };
    for (size_t i = 0; type && i < sizeof(MAP) / sizeof(MAP[0]); i++)
        if (strcmp(type, MAP[i].type) == 0) return MAP[i].kind;
    return ROW_OTHER;
}

/* The whole active branch, root→leaf. ?1 = session. */
static const char SQL_BRANCH_ROWS[] =
    "WITH RECURSIVE b(id, lvl) AS ("
    "  SELECT id, 0 FROM entries WHERE " SQL_LEAF_ROW
    "  UNION ALL"
    "  SELECT e.parent_id, b.lvl+1 FROM entries e JOIN b ON e.id=b.id"
    "   WHERE e.session_id=?1)"
    " SELECT e.id, e.parent_id, COALESCE(e.iteration_id,0), e.type,"
    "   e.tool_call_id, COALESCE(e.tokens, e.token_estimate, 0), COALESCE(e.turn_id,0),"
    "   CASE WHEN e.type='tool_call' THEN " SQL_CALL_DROPPABLE("e") " END,"
    "   CASE WHEN e.type='tool_call' THEN " SQL_CALL_UNFINISHED("e") " END"
    " FROM b JOIN entries e ON e.id=b.id AND e.session_id=?1 ORDER BY b.lvl DESC;";

static int branch_load(sqlite3 *db, int64_t session_id, Branch *out) {
    memset(out, 0, sizeof(*out));
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, SQL_BRANCH_ROWS, -1, &st, NULL) != SQLITE_OK)
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
        r->turn_id = sqlite3_column_int64(st, 6);
        r->droppable = sqlite3_column_int(st, 7);
        const char *status = (const char *)sqlite3_column_text(st, 8);
        if (status) snprintf(r->unfinished, sizeof(r->unfinished), "%s", status);
        r->edit = -1;
        out->n++;
        if (cid && !r->call_id) { rc = -1; break; }
    }
    sqlite3_finalize(st);
    if (!out->rows || rc != 0) return -1;
    out->answer = -1;
    if (out->n == 0) return 0;
    out->before_id = out->rows[0].parent_id;
    out->leaf_id = out->rows[out->n - 1].id;
    int i = out->n - 1;
    while (i > 0 && out->rows[i - 1].turn_id == out->rows[out->n - 1].turn_id) i--;
    out->newest = i;
    /* The newest turn has its answer when its newest row that is not a
     * system note (the streak guard leaves one after it) is an assistant
     * message. Mid-turn that row is a result. */
    for (i = out->n - 1; i >= out->newest && out->rows[i].kind == ROW_SYSTEM; i--) {}
    if (i >= out->newest && out->rows[i].kind == ROW_ASSISTANT) out->answer = i;
    return 0;
}

/* Check 2: a call something is still going to answer blocks whatever would
 * take it off the branch, its whole turn or iteration included. A
 * background job has its "started" result and is answered later in the
 * inbox, so it does not. */
static int still_running(const Row *r) {
    return r->kind == ROW_CALL &&
           (strcmp(r->unfinished, "pending") == 0 || strcmp(r->unfinished, "running") == 0);
}

/* The rows of one iteration of a turn, by position: it opens at a
 * reasoning, assistant message or call row of a new iteration id and runs to
 * the next such row. A result always lands after its call and before the
 * next request, so a unit keeps every call with its results — including one
 * written under an iteration id of its own (a sub-agent's answer) — and the
 * notes and summaries standing after it. */
static int opens_unit(const Row *r, int64_t current) {
    return (r->kind == ROW_REASONING || r->kind == ROW_ASSISTANT || r->kind == ROW_CALL)
        && r->iteration_id != current;
}

/* The units of rows [from, n): their first rows into starts (room for
 * n - from + 1), starts[count] = n. Rows before the first unit opened the
 * turn. Returns the count. */
static int unit_starts(const Branch *b, int from, int *starts) {
    int units = 0;
    int64_t current = -1;
    for (int i = from; i < b->n; i++)
        if (opens_unit(&b->rows[i], current)) {
            starts[units++] = i;
            current = b->rows[i].iteration_id;
        }
    starts[units] = b->n;
    return units;
}

/* ── Deciding: every edit marked on the branch, nothing written ───*/

static const char *op_name(char op) {
    switch (op) {
    case 'd': return "drop";
    case 'r': return "replace";
    case 'n': return "note";
    case 'w': return "rewrite";
    case 't': return "drop_turns";
    default:  return "summarise";
    }
}

/* The rows a call id is looked up among: the named turn, or the newest. */
static int turn_rows(const Branch *b, const Edit *e, int *lo, int *hi) {
    if (!e->has_turn) { *lo = b->newest; *hi = b->n; return 0; }
    *lo = -1;
    for (int i = 0; i < b->n; i++)
        if (b->rows[i].turn_id == e->turn_id) {
            if (*lo < 0) *lo = i;
            *hi = i + 1;
        }
    return *lo < 0 ? -1 : 0;
}

/* The one row of `kind` carrying this call id among [lo, hi): its index, -1
 * none, -2 more than one. Results pair by call id alone (see the input's
 * `res`). */
static int find_row(const Branch *b, int lo, int hi, RowKind kind, const char *call_id) {
    int found = -1;
    for (int i = lo; i < hi; i++) {
        if (b->rows[i].kind != kind || !b->rows[i].call_id ||
            strcmp(b->rows[i].call_id, call_id) != 0)
            continue;
        if (found >= 0) return -2;
        found = i;
    }
    return found;
}

static TurnEditResult find_call(const Branch *b, const Edit *e, int *ci, int *lo, int *hi,
                                char **err_out) {
    const char *verb = op_name(e->op);
    char where[48];
    if (e->has_turn) snprintf(where, sizeof(where), "turn %lld", (long long)e->turn_id);
    else snprintf(where, sizeof(where), "this turn");
    *ci = turn_rows(b, e, lo, hi) == 0 ? find_row(b, *lo, *hi, ROW_CALL, e->call_id) : -1;
    if (*ci == -1)
        return fail(err_out, TURN_EDIT_REJECTED, "%s: \"%.80s\" is not a tool call of %s",
                    verb, e->call_id, where);
    if (*ci == -2)
        return fail(err_out, TURN_EDIT_REJECTED,
                    "%s: call id \"%.80s\" names more than one call in %s", verb, e->call_id,
                    where);
    return TURN_EDIT_NOTHING;
}

/* Each row is named by one edit at most. */
static TurnEditResult claim(Branch *b, int i, int k, const char *verb, char **err_out) {
    Row *r = &b->rows[i];
    if (r->claimed && r->claimed != k + 1) {
        if (r->kind == ROW_CALL && r->call_id)
            return fail(err_out, TURN_EDIT_REJECTED,
                        "%s: call \"%.80s\" is named by two edits (edits[%d] and edits[%d])",
                        verb, r->call_id, r->claimed - 1, k);
        return fail(err_out, TURN_EDIT_REJECTED,
                    "%s: entry %lld is already named by edits[%d]", verb,
                    (long long)r->id, r->claimed - 1);
    }
    r->claimed = k + 1;
    return TURN_EDIT_NOTHING;
}

/* Rows [from, to) leave the branch for edit k. */
static TurnEditResult take_span(Branch *b, int from, int to, int k, const char *verb,
                                char **err_out) {
    for (int i = from; i < to; i++) {
        Row *r = &b->rows[i];
        if (still_running(r))
            return fail(err_out, TURN_EDIT_REJECTED,
                        "%s: call \"%.80s\" is still %s and cannot leave the branch", verb,
                        r->call_id ? r->call_id : "?", r->unfinished);
        if (i == b->answer)
            return fail(err_out, TURN_EDIT_REJECTED, "%s: the turn's answer never leaves", verb);
        TurnEditResult rc = claim(b, i, k, verb, err_out);
        if (rc != TURN_EDIT_NOTHING) return rc;
        r->remove = 1;
    }
    return TURN_EDIT_NOTHING;
}

/* drop: the call must be finished, droppable and named once. A protected
 * call (check 3) keeps its place; it may still be rewritten, and leave with
 * its whole turn or iteration. */
static TurnEditResult mark_drop(Branch *b, Edit *e, int k, char **err_out) {
    int ci, lo, hi;
    TurnEditResult rc = find_call(b, e, &ci, &lo, &hi, err_out);
    if (rc != TURN_EDIT_NOTHING) return rc;
    Row *c = &b->rows[ci];
    if (c->unfinished[0])
        return fail(err_out, TURN_EDIT_REJECTED, "drop: call \"%.80s\" is not finished (%s)",
                    e->call_id, c->unfinished);
    if (!c->droppable)
        return fail(err_out, TURN_EDIT_REJECTED,
                    "drop: call \"%.80s\" is protected and cannot be dropped", e->call_id);
    /* Iterations are told apart by id; without one there is no saying which
     * assistant message a call belongs to. */
    if (c->iteration_id <= 0)
        return fail(err_out, TURN_EDIT_REJECTED, "drop: call \"%.80s\" has no iteration id",
                    e->call_id);
    if ((rc = claim(b, ci, k, op_name(e->op), err_out)) != TURN_EDIT_NOTHING) return rc;
    c->remove = c->dropped = 1;
    for (int i = lo; i < hi; i++) {
        Row *r = &b->rows[i];
        if (r->kind != ROW_RESULT || !r->call_id || strcmp(r->call_id, e->call_id) != 0)
            continue;
        if ((rc = claim(b, i, k, op_name(e->op), err_out)) != TURN_EDIT_NOTHING) return rc;
        r->remove = 1;
    }
    return TURN_EDIT_NOTHING;
}

/* replace: a new result in the old one's place. */
static TurnEditResult mark_replace(Branch *b, Edit *e, int k, char **err_out) {
    int ci, lo, hi;
    TurnEditResult rc = find_call(b, e, &ci, &lo, &hi, err_out);
    if (rc != TURN_EDIT_NOTHING) return rc;
    if (still_running(&b->rows[ci]))
        return fail(err_out, TURN_EDIT_REJECTED, "replace: call \"%.80s\" is not finished (%s)",
                    e->call_id, b->rows[ci].unfinished);
    if ((rc = claim(b, ci, k, op_name(e->op), err_out)) != TURN_EDIT_NOTHING) return rc;
    e->row = find_row(b, lo, hi, ROW_RESULT, e->call_id);
    if (e->row < 0)
        return fail(err_out, TURN_EDIT_REJECTED, "replace: call \"%.80s\" has %s", e->call_id,
                    e->row == -2 ? "more than one result" : "no result to replace");
    if ((rc = claim(b, e->row, k, op_name(e->op), err_out)) != TURN_EDIT_NOTHING) return rc;
    b->rows[e->row].edit = k;
    return TURN_EDIT_NOTHING;
}

/* rewrite: a new row with new text — a user or assistant message, a note, a
 * summary — or a call with new arguments, in the original's place. The
 * newest turn's answer is never rewritten: it has been delivered, and it is
 * the leaf delivery keys on. */
static TurnEditResult mark_rewrite(Branch *b, Edit *e, int k, char **err_out) {
    TurnEditResult rc;
    if (!e->has_entry) {
        int ci, lo, hi;
        if ((rc = find_call(b, e, &ci, &lo, &hi, err_out)) != TURN_EDIT_NOTHING) return rc;
        if (still_running(&b->rows[ci]))
            return fail(err_out, TURN_EDIT_REJECTED,
                        "rewrite: call \"%.80s\" is not finished (%s)", e->call_id,
                        b->rows[ci].unfinished);
        e->row = ci;
    } else {
        e->row = -1;
        for (int i = 0; i < b->n && e->row < 0; i++)
            if (b->rows[i].id == e->entry_id) e->row = i;
        if (e->row < 0)
            return fail(err_out, TURN_EDIT_REJECTED,
                        "rewrite: entry %lld is not on the session's branch",
                        (long long)e->entry_id);
        RowKind kind = b->rows[e->row].kind;
        if (kind != ROW_USER && kind != ROW_ASSISTANT && kind != ROW_SYSTEM &&
            kind != ROW_SUMMARY)
            return fail(err_out, TURN_EDIT_REJECTED,
                        "rewrite: entry %lld is not a user or assistant message, a note or a"
                        " summary (a call is rewritten by call_id with args; a result is"
                        " replaced)", (long long)e->entry_id);
        if (e->row == b->answer)
            return fail(err_out, TURN_EDIT_REJECTED,
                        "rewrite: entry %lld is the turn's answer, which is never edited",
                        (long long)e->entry_id);
    }
    if ((rc = claim(b, e->row, k, op_name(e->op), err_out)) != TURN_EDIT_NOTHING) return rc;
    b->rows[e->row].edit = k;
    return TURN_EDIT_NOTHING;
}

/* A run of whole turns before the newest: drop_turns, or summarise by turn. */
static TurnEditResult turn_span(const Branch *b, const Edit *e, int *lo, int *hi,
                                char **err_out) {
    const char *verb = op_name(e->op);
    if (e->from > e->to)
        return fail(err_out, TURN_EDIT_REJECTED, "%s: from_turn %lld is after to_turn %lld —"
                    " the range runs oldest to newest", verb, (long long)e->from,
                    (long long)e->to);
    int first = -1, last = -1;
    for (int i = 0; i < b->n; i++) {
        if (b->rows[i].turn_id == e->from && first < 0) first = i;
        if (b->rows[i].turn_id == e->to) last = i;
    }
    if (first < 0 || last < 0)
        return fail(err_out, TURN_EDIT_REJECTED, "%s: turn %lld is not in this session's"
                    " context", verb, (long long)(first < 0 ? e->from : e->to));
    if (last >= b->newest)
        return fail(err_out, TURN_EDIT_REJECTED, "%s: turn %lld is the live turn — only"
                    " whole turns before it", verb, (long long)b->rows[b->newest].turn_id);
    for (int i = first; i <= last; i++)
        if (b->rows[i].turn_id < e->from || b->rows[i].turn_id > e->to)
            return fail(err_out, TURN_EDIT_REJECTED, "%s: turns %lld-%lld are not a"
                        " contiguous run of this session", verb, (long long)e->from,
                        (long long)e->to);
    *lo = first;
    *hi = last + 1;
    return TURN_EDIT_NOTHING;
}

/* A run of whole iterations of the newest turn — never its answer's. */
static TurnEditResult iteration_span(const Branch *b, const Edit *e, int *lo, int *hi,
                                     char **err_out) {
    if (e->from > e->to)
        return fail(err_out, TURN_EDIT_REJECTED, "summarise: from_iteration %lld is after"
                    " to_iteration %lld", (long long)e->from, (long long)e->to);
    int *starts = malloc((size_t)(b->n - b->newest + 1) * sizeof(int));
    if (!starts) return fail(err_out, TURN_EDIT_ERROR, "out of memory");
    int units = unit_starts(b, b->newest, starts), uf = -1, ut = -1;
    for (int u = 0; u < units; u++) {
        if (b->rows[starts[u]].iteration_id == e->from && uf < 0) uf = u;
        if (b->rows[starts[u]].iteration_id == e->to && ut < 0) ut = u;
    }
    TurnEditResult rc = TURN_EDIT_NOTHING;
    if (uf < 0 || ut < 0)
        rc = fail(err_out, TURN_EDIT_REJECTED, "summarise: iteration %lld is not an iteration"
                  " of the newest turn", (long long)(uf < 0 ? e->from : e->to));
    else if (uf > ut)
        rc = fail(err_out, TURN_EDIT_REJECTED, "summarise: from_iteration %lld comes after"
                  " to_iteration %lld on the branch", (long long)e->from, (long long)e->to);
    else if (b->answer >= 0 && ut == units - 1)
        rc = fail(err_out, TURN_EDIT_REJECTED, "summarise: iteration %lld holds the turn's"
                  " answer, which is never edited", (long long)e->to);
    else {
        /* The notes and summaries standing directly before the run — after
         * the iteration before it, or after the turn's user entries — leave
         * with it (the input lists them in `notes` with before_iteration =
         * from_iteration): a turn summarised again and again then carries
         * one summary that folds the last, not a stack of them. */
        *lo = starts[uf];
        while (*lo > b->newest &&
               (b->rows[*lo - 1].kind == ROW_SYSTEM || b->rows[*lo - 1].kind == ROW_SUMMARY))
            (*lo)--;
        *hi = starts[ut + 1];
    }
    free(starts);
    return rc;
}

/* drop_turns and summarise. A summary is an entry of its own where the run
 * was: between turns it carries the first replaced turn's id (it opens
 * nothing, and is dropped as the oldest content when the trim reaches it);
 * inside the newest turn, that turn's. */
static TurnEditResult mark_span(Branch *b, Edit *e, int k, char **err_out) {
    int lo = 0, hi = 0;
    TurnEditResult rc = e->by_turns || e->op == 't'
        ? turn_span(b, e, &lo, &hi, err_out)
        : iteration_span(b, e, &lo, &hi, err_out);
    if (rc == TURN_EDIT_NOTHING) rc = take_span(b, lo, hi, k, op_name(e->op), err_out);
    if (rc != TURN_EDIT_NOTHING) return rc;
    e->row = lo;
    e->new_turn = b->rows[lo].turn_id;
    return TURN_EDIT_NOTHING;
}

/* Rows of an iteration still on the branch: its calls, or its calls and
 * assistant message. */
static int iteration_left(const Branch *b, int64_t iteration_id, int calls_only) {
    int left = 0;
    for (int i = 0; i < b->n; i++) {
        const Row *r = &b->rows[i];
        if (r->iteration_id == iteration_id && !r->remove &&
            (r->kind == ROW_CALL || (!calls_only && r->kind == ROW_ASSISTANT)))
            left++;
    }
    return left;
}

/* Check 1, pairing, for what a dropped or rewritten row takes with it. An
 * iteration left with no call goes whole, text and reasoning included; one
 * that keeps some of its calls, or has a call or its text rewritten, loses
 * its reasoning entry: a provider's reasoning blob is the response as the
 * model wrote it (Anthropic's is the whole content array, replayed
 * verbatim), bound to the full set of calls it was produced with, and must
 * not be replayed beside a subset or an altered one. The assistant
 * message's tool_call_count is not touched: it counts the legacy
 * entries.tool_calls JSON, which typed tool_call rows never fill. The
 * newest turn's answer never leaves, whatever iteration id it carries. */
static TurnEditResult mark_iterations(Branch *b, char **err_out) {
    for (int i = 0; i < b->n; i++) {
        const Row *c = &b->rows[i];
        int rewritten = c->edit >= 0 && (c->kind == ROW_CALL || c->kind == ROW_ASSISTANT);
        if ((!c->dropped && !rewritten) || c->iteration_id <= 0) continue;
        int left = c->dropped ? iteration_left(b, c->iteration_id, 1) : 1;
        for (int j = 0; j < b->n; j++) {
            Row *r = &b->rows[j];
            if (r->iteration_id != c->iteration_id || j == b->answer || r->remove) continue;
            if (r->kind == ROW_REASONING) {
                r->remove = 1;
            } else if (!left && (r->kind == ROW_ASSISTANT || r->kind == ROW_RESULT)) {
                if (r->claimed)
                    return fail(err_out, TURN_EDIT_REJECTED,
                                "edits[%d]: entry %lld leaves with the iteration a drop"
                                " empties", r->claimed - 1, (long long)r->id);
                r->remove = 1;
            }
        }
    }
    return TURN_EDIT_NOTHING;
}

/* Is row i the first row of its iteration? Only there may a note go: any
 * other place would part an assistant message from its calls, or a call from
 * its results. Iteration ids are minted per session, so the whole branch can
 * be searched. */
static int opens_iteration(const Branch *b, int i) {
    const Row *r = &b->rows[i];
    if (r->kind != ROW_ASSISTANT && r->kind != ROW_REASONING && r->kind != ROW_CALL)
        return 0;
    for (int j = 0; j < i; j++)
        if (b->rows[j].iteration_id == r->iteration_id &&
            (b->rows[j].kind == ROW_ASSISTANT || b->rows[j].kind == ROW_REASONING ||
             b->rows[j].kind == ROW_CALL))
            return 0;
    return 1;
}

/* Where each note goes: before the iteration it names, in any turn. By
 * default before the newest turn's answer — reasoning included, so that
 * stays next to the answer — or, while the turn runs, at the end, where the
 * next response will read it. The answer is named by its own id (the input's
 * final.entry_id; its reasoning row has none there) and the note goes before
 * its reasoning all the same. Runs after the removals are marked: an
 * iteration this reply empties is no place to stand. */
static TurnEditResult place_notes(const Branch *b, Reply *reply, char **err_out) {
    int answer = b->answer;
    if (answer > 0 && b->rows[answer - 1].kind == ROW_REASONING &&
        b->rows[answer - 1].iteration_id == b->rows[answer].iteration_id)
        answer--;
    for (int k = 0; k < reply->n; k++) {
        Edit *e = &reply->e[k];
        if (e->op != 'n') continue;
        e->row = e->has_before ? -1 : answer >= 0 ? answer : b->n;
        for (int i = 0; i < b->n && e->row < 0; i++)
            if (b->rows[i].id == e->before &&
                (i == answer || i == b->answer || opens_iteration(b, i)))
                e->row = i == b->answer ? answer : i;
        if (e->row < 0)
            return fail(err_out, TURN_EDIT_REJECTED,
                        "note: \"before\" %lld is not the first entry of an iteration",
                        (long long)e->before);
        if (e->row < b->n && e->row != answer &&
            !iteration_left(b, b->rows[e->row].iteration_id, 0))
            return fail(err_out, TURN_EDIT_REJECTED,
                        "note: \"before\" %lld names an iteration this reply drops",
                        (long long)e->before);
        e->new_turn = b->rows[e->row < b->n ? e->row : b->n - 1].turn_id;
    }
    return TURN_EDIT_NOTHING;
}

/* Every edit of the reply, marked on the branch in memory: what leaves it,
 * which row has a stand-in, where each inserted entry goes. Nothing is
 * written until all of it has passed. The one place an operation is decided:
 * a new one adds its shape to SQL_REPLY_SHAPE, its fields to reply_load and
 * its marking here, and write_chain / record_write carry out the marks. */
static TurnEditResult mark_edits(Branch *b, Reply *reply, char **err_out) {
    TurnEditResult rc = TURN_EDIT_NOTHING;
    for (int k = 0; k < reply->n && rc == TURN_EDIT_NOTHING; k++) {
        Edit *e = &reply->e[k];
        switch (e->op) {
        case 'd': rc = mark_drop(b, e, k, err_out); break;
        case 'r': rc = mark_replace(b, e, k, err_out); break;
        case 'w': rc = mark_rewrite(b, e, k, err_out); break;
        case 't': case 's': rc = mark_span(b, e, k, err_out); break;
        case 'n': break;    /* placed below, once the removals are known */
        default:  rc = fail(err_out, TURN_EDIT_ERROR, "BUG: unknown op '%c'", e->op); break;
        }
    }
    if (rc == TURN_EDIT_NOTHING) rc = mark_iterations(b, err_out);
    if (rc == TURN_EDIT_NOTHING) rc = place_notes(b, reply, err_out);
    return rc;
}

/* ── Writing: one path for every edit, the script's and the trim's ─*/

/* One new entry under `parent` carrying a script's text. ?1 = the entry it
 * stands in for, or the session; ?2 parent, ?3 text, ?4 tokens, ?5 bytes,
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

/* A row standing in for entry ?1 — a replaced result, a rewritten message or
 * call: everything that says what it is, what it answers and where its
 * content came from is carried over (fence_id and network_hosts keep the
 * untrusted-content wrap on replaced network output). Not the report columns:
 * usage and cost belong to the response the original was. data keeps the
 * original's keys but its records — $.source among them, so a rewritten
 * cron prompt is still the cron's and the runaway-turn guard still reads it
 * as autonomous — and says who edited it ($.edited_by) and which original it
 * stands for ($.replaces) — the first one, through any chain of rewrites:
 * that is the mark an explorer shows as "edited" (P4), and what the call
 * tests read through (SQL_CALL_ROOT). */
static const char SQL_INSERT_STAND_IN[] =
    "INSERT INTO entries (parent_id, session_id, turn_id, iteration_id, type,"
    "  part_index, role, content, tool_calls, tool_call_id, tool_name, is_error,"
    "  stop_reason, model, network_hosts, fence_id, tool_call_count,"
    "  token_estimate, content_bytes, tokens, data)"
    " SELECT ?2, session_id, turn_id, iteration_id, type, part_index, role,"
    "  ?3, tool_calls, tool_call_id, tool_name, is_error, stop_reason, model,"
    "  network_hosts, fence_id, tool_call_count, ?4, ?5,"
    /* A measured entry keeps its measure, scaled to the new length. A short
     * one is mostly framing, which does not scale: it goes back to the
     * estimate, as does one never measured. */
    "  CASE WHEN tokens IS NOT NULL AND content_bytes >= 256"
    "       THEN tokens * ?5 / content_bytes END,"
    "  json_set(json_remove(COALESCE(data,'{}'),'$.turn_edits'),"
    "    '$.edited_by',?7,"
    "    '$.replaces'," SQL_CALL_ROOT("entries") ")"
    " FROM entries WHERE id=?1;";

/* A note is an ordinary system entry, shaped like every other one written
 * mid-session (advance.c's streak_note): type 'system', role 0, iteration 0,
 * provenance in data.source, and data.edited_by as on every entry an edit
 * wrote. Role 0 is what keeps it out of every reader that picks entries by
 * role (the runaway-turn guard reads role 1 only); the transcript sends it by
 * its type, and reads a 'hook:<extension>' source to say whose words it is
 * (llm_payload.c's SQL_SYSTEM_NOTE_PREFIX). ?1 = session id. */
static const char SQL_INSERT_NOTE[] =
    "INSERT INTO entries (parent_id, session_id, turn_id, iteration_id, type, role,"
    "  content, token_estimate, content_bytes, data)"
    " VALUES (?2, ?1, ?6, 0, 'system', 0, ?3, ?4, ?5,"
    "         json_object('source',?7,'edited_by',?7));";

/* A summary is a compaction entry, as session_condense's and the old
 * compaction's are: sent as user text under '[Summary of earlier
 * conversation]', no iteration (it stands alone on the wire). Role 4, so no
 * reader that counts user messages (the runaway-turn guard, the cron failure
 * streak) takes it for one. ?1 = session. */
static const char SQL_INSERT_SUMMARY[] =
    "INSERT INTO entries (parent_id, session_id, turn_id, iteration_id, type, role,"
    "  content, stop_reason, token_estimate, content_bytes, tool_call_count, data)"
    " VALUES (?2, ?1, ?6, NULL, 'compaction', 4, ?3, 0, ?4, ?5, 0,"
    "         json_object('source',?7,'edited_by',?7));";

/* The new branch as written, root→leaf, and the first row of it that is new
 * or hangs off something new: everything before that row is as it was. */
typedef struct {
    int64_t *ids;
    int n, cap;
    int64_t cut;
} Chain;

static int chain_add(Chain *c, int64_t id, int changed) {
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        int64_t *tmp = realloc(c->ids, (size_t)cap * sizeof(int64_t));
        if (!tmp) return -1;
        c->ids = tmp;
        c->cap = cap;
    }
    c->ids[c->n++] = id;
    if (changed && !c->cut) c->cut = id;
    return 0;
}

static int write_inserts(sqlite3 *db, int64_t session_id, Reply *reply, int at,
                         const char *label, int64_t *prev, Chain *out) {
    for (int k = 0; k < reply->n; k++) {
        Edit *e = &reply->e[k];
        if (!IS_INSERT(e) || e->row != at) continue;
        e->new_id = insert_text(db, e->op == 'n' ? SQL_INSERT_NOTE : SQL_INSERT_SUMMARY,
                                session_id, *prev, e->text, e->new_turn, label);
        if (e->new_id <= 0 || chain_add(out, e->new_id, 1) != 0) return -1;
        *prev = e->new_id;
    }
    return 0;
}

/* Rebuild the branch front to back: every row that stays hangs off the row
 * that now precedes it, stand-ins and inserted entries in their places. A
 * re-pointed row remembers the parent it was written under — the first one
 * only, so the chain as written can always be walked back (a row that
 * leaves the branch is not touched and still points at its own original
 * parent). The leaf is the last row of the new chain: the newest turn's
 * answer never leaves, so at a turn's end it is unchanged. */
static int write_chain(sqlite3 *db, int64_t session_id, const Branch *b, Reply *reply,
                       const char *label, Chain *out) {
    int64_t prev = b->before_id;
    for (int i = 0; i <= b->n; i++) {
        if (write_inserts(db, session_id, reply, i, label, &prev, out) != 0) return -1;
        if (i == b->n) break;
        const Row *r = &b->rows[i];
        if (r->remove) continue;
        int64_t id = r->id;
        int changed = 0;
        if (r->edit >= 0) {
            Edit *e = &reply->e[r->edit];
            id = e->new_id = insert_text(db, SQL_INSERT_STAND_IN, r->id, prev, e->text, 0, label);
            if (id <= 0) return -1;
            changed = 1;
        } else if (r->parent_id != prev) {
            if (db_exec_ids(db, "UPDATE entries SET"
                                " original_parent_id=COALESCE(original_parent_id, parent_id),"
                                " parent_id=?2 WHERE id=?1;", r->id, prev, 0) != 0)
                return -1;
            changed = 1;
        }
        if (chain_add(out, id, changed) != 0) return -1;
        prev = id;
    }
    /* Each insert moved the leaf to itself (entries_leaf_ai). */
    return out->n > 0 ? db_exec_ids(db, "UPDATE sessions SET leaf_id=?2 WHERE id=?1;",
                                    session_id, prev, 0)
                      : -1;
}

/* Proof before COMMIT: walked from the leaf, the branch is exactly the rows
 * meant to be on it, in order, ending at the same root. */
static int chain_sound(sqlite3 *db, int64_t session_id, const Branch *was, const Chain *c) {
    Branch now = {0};
    int ok = branch_load(db, session_id, &now) == 0 && now.n == c->n && now.n > 0 &&
             now.before_id == was->before_id;
    for (int i = 0; ok && i < now.n; i++) ok = now.rows[i].id == c->ids[i];
    branch_free(&now);
    return ok;
}

/* The branch's size as written now: every entry at its stored count or its
 * estimate. */
static int branch_tokens(sqlite3 *db, int64_t session_id) {
    return (int)db_scalar_i64(db,
        "WITH RECURSIVE w(id) AS (SELECT leaf_id FROM sessions WHERE id=?1"
        "  UNION ALL SELECT e.parent_id FROM entries e JOIN w ON e.id=w.id"
        "   WHERE e.session_id=?1)"
        " SELECT COALESCE(SUM(COALESCE(e.tokens, e.token_estimate)),0)"
        "   FROM w JOIN entries e ON e.id=w.id AND e.session_id=?1;", session_id, -1);
}

/* ── The record (check 4) ──────────────────────────────────────────
 *
 * One object appended to $.turn_edits in the data of the host entry — the
 * session's leaf after a script's edit (at a turn's end, its answer), the
 * note the trim wrote — other keys there kept:
 *
 *   by, at          who and when
 *   left_branch     every original that left, in chain order (replaced and
 *                   rewritten originals among them)
 *   replaced        {original, replacement} for each stand-in, in edit order
 *   notes,          the entries written, in edit order
 *   summaries
 *   tokens          the branch's size before and after (the trim: the
 *                   session's, head included)
 *   thinking_cut    the first entry of the new branch that is new or
 *                   re-pointed: reasoning made before this edit, at or after
 *                   it, is not replayed (llm_payload.c). null when only the
 *                   tail went.
 *
 * The host is always an entry at least as new as every report on the
 * branch, so "a record on an entry not older than the previous report" is
 * how usage attribution and the head measure tell that an edit came between
 * (db_response.c, context.c). Never sent to the model. */

static void ids_json(Buf *b, const int64_t *ids, int n) {
    buf_append_char(b, '[');
    for (int i = 0; i < n; i++) buf_appendf(b, "%s%lld", i ? "," : "", (long long)ids[i]);
    buf_append_char(b, ']');
}

static int record_write(sqlite3 *db, int64_t host, const char *label, const Branch *b,
                        const Reply *reply, int before, int after, int64_t cut,
                        const char *extra) {
    Buf left = {0}, repl = {0}, notes = {0}, sums = {0};
    int64_t *ids = malloc((size_t)(b->n + reply->n + 1) * sizeof(int64_t));
    if (!ids) return -1;
    int n = 0;
    for (int i = 0; i < b->n; i++)
        if (b->rows[i].remove || b->rows[i].edit >= 0) ids[n++] = b->rows[i].id;
    ids_json(&left, ids, n);
    buf_append_char(&repl, '[');
    for (int k = 0, m = 0; k < reply->n; k++)
        if (reply->e[k].op == 'r' || reply->e[k].op == 'w')
            buf_appendf(&repl, "%s{\"original\":%lld,\"replacement\":%lld}", m++ ? "," : "",
                        (long long)b->rows[reply->e[k].row].id, (long long)reply->e[k].new_id);
    buf_append_char(&repl, ']');
    for (int pass = 0; pass < 2; pass++) {
        n = 0;
        for (int k = 0; k < reply->n; k++)
            if (reply->e[k].op == (pass ? 's' : 'n')) ids[n++] = reply->e[k].new_id;
        ids_json(pass ? &sums : &notes, ids, n);
    }
    free(ids);
    char *l = buf_take(&left), *r = buf_take(&repl), *no = buf_take(&notes), *s = buf_take(&sums);
    int rc = -1;
    sqlite3_stmt *st;
    if (l && r && no && s && sqlite3_prepare_v2(db,
            "UPDATE entries SET data=json_insert("
            "   CASE WHEN json_type(data,'$.turn_edits') IS 'array' THEN data"
            "        ELSE json_set(COALESCE(data,'{}'),'$.turn_edits',json('[]')) END,"
            "   '$.turn_edits[#]', json((SELECT CASE WHEN rec IS NULL THEN obj"
            "       ELSE json_insert(obj,'$.record',json(rec)) END FROM (SELECT"
            "     json_extract(?10,'$.record') AS rec,"
            "     json_patch(json_object('by',?2,'at',unixepoch(),'left_branch',json(?3),"
            "       'replaced',json(?4),'notes',json(?5),'summaries',json(?6),"
            "       'tokens',json_object('before',?7,'after',?8),'thinking_cut',?9),"
            "       COALESCE(?11,'{}')) AS obj))))"
            " WHERE id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, host);
        sqlite3_bind_text(st, 2, label, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, l, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, r, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, no, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 6, s, -1, SQLITE_STATIC);
        sqlite3_bind_int(st, 7, before);
        sqlite3_bind_int(st, 8, after);
        if (cut > 0) sqlite3_bind_int64(st, 9, cut);
        if (reply->json) sqlite3_bind_text(st, 10, reply->json, -1, SQLITE_STATIC);
        if (extra) sqlite3_bind_text(st, 11, extra, -1, SQLITE_STATIC);
        rc = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1 ? 0 : -1;
        sqlite3_finalize(st);
    }
    free(l); free(r); free(no); free(s);
    return rc;
}

/* Everything that needs the branch: runs inside the write transaction, so
 * what is checked is what is edited. */
/* What an applied edit did, for its log line. */
typedef struct {
    int before, after;          /* the branch's tokens */
    int left, replaced, notes, summaries;
} EditTally;

static TurnEditResult apply_locked(sqlite3 *db, int64_t session_id, Reply *reply,
                                   const char *label, EditTally *t, char **err_out) {
    Branch b = {0};
    Chain c = {0};
    TurnEditResult rc;
    if (branch_load(db, session_id, &b) != 0) {
        rc = fail(err_out, TURN_EDIT_ERROR, "could not read the session: %s",
                  sqlite3_errmsg(db));
        goto out;
    }
    if (b.n == 0) {
        rc = fail(err_out, TURN_EDIT_REJECTED, "the session has no turn to edit");
        goto out;
    }
    if ((rc = mark_edits(&b, reply, err_out)) != TURN_EDIT_NOTHING) goto out;

    int before = 0, after;
    for (int i = 0; i < b.n; i++) before += b.rows[i].tokens;
    if (write_chain(db, session_id, &b, reply, label, &c) != 0 ||
        (after = branch_tokens(db, session_id)) < 0 ||
        record_write(db, c.ids[c.n - 1], label, &b, reply, before, after, c.cut, NULL) != 0) {
        rc = fail(err_out, TURN_EDIT_ERROR, "write failed: %s", sqlite3_errmsg(db));
        goto out;
    }
    if (!chain_sound(db, session_id, &b, &c)) {
        rc = fail(err_out, TURN_EDIT_ERROR,
                  "BUG: the edited branch is not the expected chain — nothing applied");
        LOG_ERROR_("turn edit session=%lld: edited branch failed its check",
                   (long long)session_id);
        goto out;
    }
    t->before = before;
    t->after = after;
    for (int i = 0; i < b.n; i++) t->left += b.rows[i].remove || b.rows[i].edit >= 0;
    for (int k = 0; k < reply->n; k++) {
        char op = reply->e[k].op;
        t->replaced += op == 'r' || op == 'w';
        t->notes += op == 'n';
        t->summaries += op == 's';
    }
    rc = TURN_EDIT_APPLIED;
out:
    free(c.ids);
    branch_free(&b);
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
    EditTally t = {0};
    rc = apply_locked(db, session_id, &reply, source_label, &t, err_out);
    if (rc != TURN_EDIT_APPLIED ||
        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        if (rc == TURN_EDIT_APPLIED)
            rc = fail(err_out, TURN_EDIT_ERROR, "COMMIT failed: %s", sqlite3_errmsg(db));
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    } else {
        /* One line per edit, whoever sent it — the trim logs its own. */
        LOG_INFO_("edit session=%lld source=%s tokens=%d->%d left=%d replaced=%d"
                  " notes=%d summaries=%d", (long long)session_id,
                  source_label ? source_label : "?", t.before, t.after, t.left,
                  t.replaced, t.notes, t.summaries);
    }
    reply_free(&reply);
    return rc;
}

/* ── The trim: C's fallback ────────────────────────────────────────
 *
 * When the session is over its trim threshold at the end of an iteration
 * and nothing else got it under, C cuts it to the prune target with no model
 * call and no policy beyond those two numbers:
 *
 *   1. drop the oldest whole turns (never the running one);
 *   2. if still over, drop the oldest whole iterations of the running turn,
 *      keeping the user entries that opened it and its newest iteration;
 *   3. one note where things were dropped, carrying the record;
 *   4. if still over the threshold, say so — the caller stops the turn.
 *
 * The rules are the ones every edit obeys, and the writing is theirs
 * (write_chain, record_write, chain_sound): a call leaves only with its
 * result (whole turns and whole iterations guarantee it), a call still
 * pending or running blocks the removal of whatever holds it, and originals
 * are re-pointed or added to, never modified or deleted. Sizes are what the
 * next request would carry (context_measure): an entry's stored count or
 * its estimate, 0 for a response that stopped on an error and its results. */

static int span_blocked(const Branch *b, int from, int to) {
    for (int i = from; i < to; i++)
        if (still_running(&b->rows[i])) return 1;
    return 0;
}

static int span_drop(Branch *b, int from, int to) {
    int size = 0;
    for (int i = from; i < to; i++) {
        b->rows[i].remove = 1;
        size += b->rows[i].tokens;
    }
    return size;
}

/* Steps 1 and 2. *total is lowered by what leaves. */
static void trim_mark(Branch *b, int *total, int target, TrimReport *rep) {
    int last = b->newest;
    for (int i = 0; i < last && *total > target;) {
        int j = i;
        while (j < last && b->rows[j].turn_id == b->rows[i].turn_id) j++;
        if (!span_blocked(b, i, j)) {
            *total -= span_drop(b, i, j);
            rep->turns++;
        }
        i = j;
    }
    if (*total <= target) return;

    /* Units of the running turn: everything before the first is what
     * opened it, and stays. The newest unit stays too. */
    int *starts = malloc((size_t)(b->n - last + 1) * sizeof(int));
    if (!starts) return;
    int units = unit_starts(b, last, starts);
    for (int u = 0; u + 1 < units && *total > target; u++) {
        if (span_blocked(b, starts[u], starts[u + 1])) continue;
        *total -= span_drop(b, starts[u], starts[u + 1]);
        rep->iterations++;
    }
    free(starts);
}

/* Where the note goes: before the first row that stays after the first one
 * that left — inside the running turn when its iterations were dropped (the
 * gap the model most needs explained), else where the turns were. */
static int trim_note_at(const Branch *b, int from) {
    int i = from;
    while (i < b->n && !b->rows[i].remove) i++;
    while (i < b->n && b->rows[i].remove) i++;
    return i < b->n ? i : -1;
}

static void trim_note_text(char *buf, size_t cap, int n_turns, int n_steps) {
    char turns[64] = "", steps[96] = "";
    if (n_turns)
        snprintf(turns, sizeof(turns), "%d earlier turn%s", n_turns, n_turns == 1 ? "" : "s");
    if (n_steps)
        snprintf(steps, sizeof(steps), "%d earlier step%s of this turn", n_steps,
                 n_steps == 1 ? "" : "s");
    snprintf(buf, cap, "%s%s%s %s dropped to fit the context. What they said is no"
             " longer in front of you; check before repeating an action.",
             turns, turns[0] && steps[0] ? " and " : "", steps,
             n_turns + n_steps == 1 ? "was" : "were");
}

/* A long turn is trimmed again and again, and each trim's note would stay
 * among the rows that opened it. The new note replaces those: an earlier
 * trim note there leaves the branch and its running total (the record's
 * `total`, which is what its text said — `dropped` is that trim alone) is
 * carried into the new note's text, so the model reads one running total. */
static void trim_fold_notes(sqlite3 *db, Branch *b, int *total, int *n_turns, int *n_steps) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(data,'$.turn_edits[0].total.turns'),"
            "       json_extract(data,'$.turn_edits[0].total.iterations')"
            "  FROM entries WHERE id=?1 AND type='system' AND json_valid(data)"
            "   AND json_extract(data,'$.source')='" TRIM_SOURCE "'"
            /* A trim note a script rewrote is the script's text now. */
            "   AND json_extract(data,'$.replaces') IS NULL;",
            -1, &st, NULL) != SQLITE_OK)
        return;
    for (int i = b->newest; i < b->n && !opens_unit(&b->rows[i], -1); i++) {
        if (b->rows[i].kind != ROW_SYSTEM) continue;
        sqlite3_reset(st);
        sqlite3_bind_int64(st, 1, b->rows[i].id);
        if (sqlite3_step(st) != SQLITE_ROW) continue;
        *n_turns += sqlite3_column_int(st, 0);
        *n_steps += sqlite3_column_int(st, 1);
        *total -= span_drop(b, i, i + 1);
    }
    sqlite3_finalize(st);
}

/* The size the next request would carry: the head (measured when a usable
 * report says so, else `head`) plus every entry it would send. */
static int trim_measure(sqlite3 *db, int64_t session_id, int head, ContextPlan *m) {
    if (context_measure(db, session_id, head, m) != 0) return -1;
    int total = m->head;
    for (int i = 0; i < m->count; i++) total += m->entries[i].tokens;
    return total;
}

static TrimResult trim_locked(sqlite3 *db, int64_t session_id, int head,
                              const ContextLimits *lim, TrimReport *rep) {
    ContextPlan m = {0};
    Branch b = {0};
    Chain c = {0};
    TrimResult rc = TRIM_ERROR;
    int total = trim_measure(db, session_id, head, &m);
    if (total < 0) goto out;
    rep->before = rep->after = total;
    if (total <= lim->threshold) { rc = TRIM_UNDER; goto out; }
    if (branch_load(db, session_id, &b) != 0 || b.n == 0) goto out;

    /* Each row at what the next request would carry for it: the measure's
     * size, 0 for a row it leaves out (both lists are in branch order). */
    for (int i = 0, j = 0; i < b.n; i++) {
        if (j < m.count && m.entries[j].id == b.rows[i].id) b.rows[i].tokens = m.entries[j++].tokens;
        else b.rows[i].tokens = 0;
    }

    /* A target at or over the threshold would leave the session over it:
     * cut to the threshold instead, whatever the numbers say. */
    int target = lim->target < lim->threshold ? lim->target : lim->threshold;
    trim_mark(&b, &total, target, rep);
    rep->after = total;
    if (rep->turns + rep->iterations == 0) { rc = TRIM_OVER; goto out; }

    int n_turns = rep->turns, n_steps = rep->iterations;
    if (rep->iterations) trim_fold_notes(db, &b, &total, &n_turns, &n_steps);
    rep->after = total;
    char text[320];
    trim_note_text(text, sizeof(text), n_turns, n_steps);
    Edit note = { .op = 'n', .text = text, .row = trim_note_at(&b, rep->iterations ? b.newest : 0) };
    Reply reply = { .json = NULL, .e = &note, .n = 1 };
    if (note.row < 0) goto out;
    note.new_turn = b.rows[note.row].turn_id;
    char extra[256];
    if (write_chain(db, session_id, &b, &reply, TRIM_SOURCE, &c) != 0) goto out;
    rep->note_id = note.new_id;
    snprintf(extra, sizeof(extra),
             "{\"dropped\":{\"turns\":%d,\"iterations\":%d},"
             "\"total\":{\"turns\":%d,\"iterations\":%d},"
             "\"limits\":{\"window\":%d,\"threshold\":%d,\"target\":%d}}",
             rep->turns, rep->iterations, n_turns, n_steps,
             lim->window, lim->threshold, lim->target);
    if (record_write(db, note.new_id, TRIM_SOURCE, &b, &reply, rep->before, rep->after, c.cut,
                     extra) != 0)
        goto out;
    if (!chain_sound(db, session_id, &b, &c)) {
        LOG_ERROR_("BUG: trim session=%lld: the branch is not the expected chain"
                   " — nothing applied", (long long)session_id);
        goto out;
    }
    rc = total > lim->threshold ? TRIM_OVER : TRIM_APPLIED;
out:
    free(c.ids);
    context_plan_free(&m);
    branch_free(&b);
    return rc;
}

TrimResult turn_edit_trim(sqlite3 *db, int64_t session_id, int head,
                          const ContextLimits *lim, TrimReport *rep) {
    memset(rep, 0, sizeof(*rep));
    if (!db || !lim || lim->threshold <= 0) return TRIM_UNDER;
    if (!sqlite3_get_autocommit(db)) {
        LOG_ERROR_("BUG: turn_edit_trim called inside a transaction");
        return TRIM_ERROR;
    }
    /* The common case reads and goes: under the threshold, no transaction. */
    ContextPlan m = {0};
    int total = trim_measure(db, session_id, head, &m);
    context_plan_free(&m);
    if (total < 0) return TRIM_ERROR;
    rep->before = rep->after = total;
    if (total <= lim->threshold) return TRIM_UNDER;

    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, NULL) != SQLITE_OK) return TRIM_ERROR;
    memset(rep, 0, sizeof(*rep));
    TrimResult rc = trim_locked(db, session_id, head, lim, rep);
    int wrote = rep->turns + rep->iterations > 0 && rc != TRIM_ERROR;
    if (!wrote || sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        if (wrote) rc = TRIM_ERROR;
        if (rc == TRIM_ERROR) { rep->turns = rep->iterations = 0; rep->note_id = 0; }
    }
    if (wrote && rc != TRIM_ERROR)
        LOG_INFO_("trim session=%lld source=" TRIM_SOURCE " turns=%d iterations=%d"
                  " tokens=%d->%d threshold=%d target=%d note=%lld",
                  (long long)session_id, rep->turns, rep->iterations, rep->before,
                  rep->after, lim->threshold, lim->target, (long long)rep->note_id);
    return rc;
}

/* The agent's governing limits and head estimate, as the C check uses them. */
static int check_inputs(sqlite3 *db, int64_t session_id, const char *agent,
                        ContextLimits *lim, int *head) {
    memset(lim, 0, sizeof(*lim));
    Config *cfg = config_load(db);
    if (!cfg) return -1;
    context_limits(db, agent, cfg->context_window, lim);
    *head = context_head_estimate(db, agent, session_id, cfg, NULL);
    config_free(cfg);
    return 0;
}

int turn_edit_measure(sqlite3 *db, int64_t session_id, const char *agent,
                      ContextLimits *lim_out) {
    ContextLimits lim;
    int head;
    if (check_inputs(db, session_id, agent, &lim, &head) != 0) return -1;
    if (lim_out) *lim_out = lim;
    ContextPlan m = {0};
    int total = trim_measure(db, session_id, head, &m);
    context_plan_free(&m);
    return total;
}

TrimResult turn_edit_trim_check(sqlite3 *db, int64_t session_id, const char *agent,
                                TrimReport *rep, ContextLimits *lim_out) {
    ContextLimits lim;
    int head;
    memset(rep, 0, sizeof(*rep));
    if (check_inputs(db, session_id, agent, &lim, &head) != 0) return TRIM_ERROR;
    if (lim_out) *lim_out = lim;
    return turn_edit_trim(db, session_id, head, &lim, rep);
}
