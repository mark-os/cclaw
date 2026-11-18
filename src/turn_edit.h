#ifndef CCLAW_TURN_EDIT_H
#define CCLAW_TURN_EDIT_H

/* Turn edit — the data side of a hook: the session's newest turn (and as
 * much of the conversation before it as the hook asked for) as plain data
 * for a script, and the checks and tree surgery for the edits it sends back,
 * on any turn of the active branch: drop a call with its result, replace a
 * result, rewrite a message or a call's arguments, insert a note, drop or
 * summarise a run of whole turns, summarise whole iterations of the newest
 * turn — between iterations of a running turn as well as at a turn's end.
 * What an edit is for is the script's business; C checks structure and
 * keeps the record, never content: (1) a call and its result leave
 * together, (2) a call still pending or running blocks whatever would
 * remove it, (3) a call made for its effect is never dropped on its own,
 * (4) originals are never modified or deleted, and every edit appends a
 * record. Nothing here runs a script or changes session state. C's own trim
 * (turn_edit_trim) and session_condense go through the same checks and the
 * same writing.
 */

#include "context.h"
#include "sqlite3.h"
#include <stdint.h>

/* What an edit leaves in entries (P4, for anyone showing a session):
 *   - every entry an edit wrote — a note, a summary, a stand-in — has
 *     data.edited_by = the edit's label ('fallback' for the trim,
 *     'hook:<ext>', 'tool:session_condense');
 *   - a note or a summary has data.source = the same label: the edit is
 *     where its words came from;
 *   - a stand-in (a replaced result, a rewritten message or call) keeps the
 *     original's data.source — a rewritten cron prompt is still the cron's —
 *     and has data.replaces = the id of the first original it stands for,
 *     through any chain of rewrites: "edited" is TURN_EDIT_SQL_STANDS_IN(data);
 *   - an original re-pointed past removed rows has original_parent_id;
 *   - each edit appends its record to $.turn_edits on one entry (the leaf
 *     after a script's edit, the trim's note): by, at, left_branch,
 *     replaced [{original, replacement}], notes, summaries,
 *     tokens {before, after}, thinking_cut, and the script's own "record". */
#define TURN_EDIT_SQL_STANDS_IN(data) \
    "(CASE WHEN json_valid(" data ") THEN json_extract(" data ",'$.replaces') END IS NOT NULL)"

/* Largest "record" a reply may ask to keep, in bytes of JSON. A sanity cap:
 * the record is bookkeeping on one entry, not storage. */
#define TURN_EDIT_RECORD_MAX_BYTES (16 * 1024)

typedef enum {
    TURN_EDIT_APPLIED  = 1,   /* the turn was edited and the record written */
    TURN_EDIT_NOTHING  = 0,   /* the reply asked for no edit; nothing written */
    TURN_EDIT_REJECTED = -1,  /* the reply or the turn failed a check; nothing written */
    TURN_EDIT_ERROR    = -2,  /* our side failed (DB, memory); nothing written */
} TurnEditResult;

/* What a hook asked to be sent: its manifest's "input" object (hooks.input),
 * with the defaults filled in (hook_next). The newest turn is always sent in
 * full; these say how much of the conversation before it. */
typedef struct {
    int history;            /* earlier turns: 0 none, N the last N, TURN_EDIT_HISTORY_ALL */
    int history_results;    /* 0: earlier turns' results come without their text */
    int max_bytes;          /* the most the whole input may be, in bytes of JSON */
} TurnEditInputSpec;

#define TURN_EDIT_HISTORY_ALL (-1)

/* max_bytes when the manifest does not say: about what a script on the
 * default heap (js_heap_mb 8) can JSON-parse — the parsed value costs several
 * times its text. A script that asks for more raises its heap too. */
#define TURN_EDIT_INPUT_DEFAULT_BYTES (1024 * 1024)

/* The least max_bytes may be: below this the fixed JSON around the smallest
 * turn (one question, one answer) already does not fit, so the hook could
 * never be started. Promote refuses less. */
#define TURN_EDIT_INPUT_MIN_BYTES 1024

/* The most max_bytes may be: the largest input the hook vehicle carries
 * (RUNTOOL_ARG_FD_MAX; settle.c asserts they agree). Promote refuses more. */
#define TURN_EDIT_INPUT_MAX_BYTES (16 * 1024 * 1024)

/* A manifest that declares no "input": the finished turn alone. */
#define TURN_EDIT_INPUT_DEFAULTS \
    ((TurnEditInputSpec){ .history = 0, .history_results = 1, \
                          .max_bytes = TURN_EDIT_INPUT_DEFAULT_BYTES })

/* What the input is for, besides what the hook asked for. */
typedef struct {
    const char *event;      /* the event, sent as "event" */
    int need_answer;        /* 1: nothing is built unless the newest turn ended
                             * with an answer (the stop events); 0: a turn still
                             * running is sent as it stands, "final" null */
    const char *context;    /* a JSON object sent as "context" (the numbers a
                             * ContextThreshold script decides on), or NULL */
} TurnEditInputFor;

/* The session as a hook's input (JSON text, caller frees), stored text
 * verbatim: the newest turn on the active branch at the top level, always
 * whole — and under "history" as much of the
 * branch before it as `spec` asks for and max_bytes leaves room for, oldest
 * first: earlier turns in the same shape, and compaction summaries where they
 * stand. History that does not fit loses its oldest turns first, whole.
 * Every entry carries its size in tokens (its stored count, else its
 * estimate). The returned text is never longer than spec->max_bytes.
 *
 * NULL when the session has no entry, when what->need_answer and the newest
 * turn did not end with an assistant message, or when the newest turn alone
 * is larger than max_bytes — then *too_big (when given) is set to 1. */
char *turn_edit_input(sqlite3 *db, int64_t session_id, const TurnEditInputSpec *spec,
                      const TurnEditInputFor *what, int *too_big);

/* Validate a reply against the session's active branch and apply it, all
 * or nothing, in one BEGIN IMMEDIATE transaction with the branch walked again
 * before COMMIT — so the handle must not have a transaction open.
 * source_label names who sent the reply ("hook:<ext>",
 * "tool:session_condense") in the record and on every entry written
 * (data.edited_by; data.source on a note or summary). On REJECTED or ERROR *err_out (when given) gets a one-line
 * reason the caller frees; it is NULL otherwise. */
TurnEditResult turn_edit_apply(sqlite3 *db, int64_t session_id,
                               const char *reply_json,
                               const char *source_label, char **err_out);

/* ── The trim (C's fallback) ──────────────────────────────────────── */

typedef enum {
    TRIM_UNDER   = 0,   /* the session is at or under its threshold; nothing written */
    TRIM_APPLIED = 1,   /* trimmed, and now under the threshold */
    TRIM_OVER    = 2,   /* still over the threshold after everything removable
                         * left (possibly nothing could): the turn must stop */
    TRIM_ERROR   = -1,  /* our side failed; nothing written */
} TrimResult;

typedef struct {
    int turns;          /* whole earlier turns dropped */
    int iterations;     /* whole iterations of the running turn dropped */
    int before, after;  /* the session's size (head + entries), in tokens */
    int64_t note_id;    /* the note written, 0 when none */
} TrimReport;

/* Bring the session under lim->threshold when it is over: drop the oldest
 * whole turns down to lim->target, then the oldest whole iterations of the
 * running turn (its opening user entries and newest iteration always stay),
 * and leave one system note (data.source TRIM_SOURCE) where things went,
 * carrying the record in $.turn_edits. A call not yet 'done' keeps whatever
 * holds it. No model call. `head` is the estimated head (tools + system
 * prompt); a usable provider measure replaces it (context_measure). Reads in
 * autocommit and returns when under; writes in one BEGIN IMMEDIATE, so the
 * handle must not have a transaction open. */
TrimResult turn_edit_trim(sqlite3 *db, int64_t session_id, int head,
                          const ContextLimits *lim, TrimReport *rep);

/* The session's size as the C check sees it (head + entries, in tokens),
 * and the agent's governing limits in *lim_out (when given). -1 on error. */
int turn_edit_measure(sqlite3 *db, int64_t session_id, const char *agent,
                      ContextLimits *lim_out);

/* The C check: turn_edit_trim with the agent's governing limits
 * (context_limits) and head estimate resolved here. *lim_out, when given,
 * gets the limits used. */
TrimResult turn_edit_trim_check(sqlite3 *db, int64_t session_id, const char *agent,
                                TrimReport *rep, ContextLimits *lim_out);

#endif
