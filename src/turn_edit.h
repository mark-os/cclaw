#ifndef CCLAW_TURN_EDIT_H
#define CCLAW_TURN_EDIT_H

/* Turn edit — the data side of a turnSettled hook: the finished turn as
 * plain data for a script, and the checks and tree surgery for the edits it
 * sends back (drop a tool call with its result, replace a result's text,
 * insert a system note). What an edit is for is the script's business; only
 * what must not be left to an untrusted script lives here. Nothing here runs
 * a script or changes session state.
 */

#include "sqlite3.h"
#include <stdint.h>

/* Largest "record" a reply may ask to keep, in bytes of JSON. A sanity cap:
 * the record is bookkeeping on one entry, not storage. */
#define TURN_EDIT_RECORD_MAX_BYTES (16 * 1024)

typedef enum {
    TURN_EDIT_APPLIED  = 1,   /* the turn was edited and the record written */
    TURN_EDIT_NOTHING  = 0,   /* the reply asked for no edit; nothing written */
    TURN_EDIT_REJECTED = -1,  /* the reply or the turn failed a check; nothing written */
    TURN_EDIT_ERROR    = -2,  /* our side failed (DB, memory); nothing written */
} TurnEditResult;

/* What a turnSettled hook asked to be sent: its manifest's "input" object
 * (hooks.input), with the defaults filled in (hook_next). The finished turn is
 * always sent in full; these say how much of the conversation before it. */
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

/* The session as a hook's input (JSON text, caller frees), stored text
 * verbatim: the newest turn on the active branch at the top level — the one
 * turn an edit may touch, always whole — and under "history" as much of the
 * branch before it as `spec` asks for and max_bytes leaves room for, oldest
 * first: earlier turns in the same shape, and compaction summaries where they
 * stand. History that does not fit loses its oldest turns first, whole. The
 * returned text is never longer than spec->max_bytes.
 *
 * NULL when the session's leaf is not an assistant message (no finished
 * turn), or the finished turn alone is larger than max_bytes — then
 * *too_big (when given) is set to 1. */
char *turn_edit_input(sqlite3 *db, int64_t session_id, const TurnEditInputSpec *spec,
                      int *too_big);

/* Validate a reply against the finished turn and apply it, all or nothing,
 * in one BEGIN IMMEDIATE transaction — so the handle must not have a
 * transaction open. source_label names who sent the reply ("hook:<ext>") in
 * the record and on inserted notes. On REJECTED or ERROR *err_out (when
 * given) gets a one-line reason the caller frees; it is NULL otherwise. */
TurnEditResult turn_edit_apply(sqlite3 *db, int64_t session_id,
                               const char *reply_json,
                               const char *source_label, char **err_out);

#endif
