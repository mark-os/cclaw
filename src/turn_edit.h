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

/* Most stored text (entries.content_bytes, summed over the active branch) a
 * session may hold and still be handed to a script. The same figure as the
 * largest input the hook vehicle carries (RUNTOOL_ARG_FD_MAX; settle.c
 * asserts they agree): a bigger branch could not be delivered anyway, so it
 * is not built. */
#define TURN_EDIT_INPUT_MAX_BYTES (16 * 1024 * 1024)

/* The session as a hook's input (JSON text, caller frees), stored text
 * verbatim: the newest turn on the active branch at the top level — the one
 * turn an edit may touch — and under "history" the rest of that branch,
 * oldest first: every earlier turn in the same shape, and each compaction
 * summary where it stands. NULL when the session's leaf is not an assistant
 * message — there is no finished turn — or the branch holds more than
 * TURN_EDIT_INPUT_MAX_BYTES. */
char *turn_edit_input(sqlite3 *db, int64_t session_id);

/* Validate a reply against the finished turn and apply it, all or nothing,
 * in one BEGIN IMMEDIATE transaction — so the handle must not have a
 * transaction open. source_label names who sent the reply ("hook:<ext>") in
 * the record and on inserted notes. On REJECTED or ERROR *err_out (when
 * given) gets a one-line reason the caller frees; it is NULL otherwise. */
TurnEditResult turn_edit_apply(sqlite3 *db, int64_t session_id,
                               const char *reply_json,
                               const char *source_label, char **err_out);

#endif
