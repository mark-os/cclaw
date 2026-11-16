#ifndef CCLAW_TURN_REVIEW_H
#define CCLAW_TURN_REVIEW_H

/* Turn review — the data side of editing the turn that just finished:
 * build what a reviewer is shown, and validate and apply what it sends back
 * (drop a tool call with its result, replace a result with an excerpt, leave
 * one short note). Nothing here runs a reviewer or changes session state;
 * see specs/turn-review.md.
 */

#include "sqlite3.h"
#include <stdint.h>

/* Longest note a review may leave, in characters. */
#define TURN_REVIEW_NOTE_MAX_CHARS 600

/* The line that stands for a run of omitted lines in a trimmed result:
 * the number of lines, then the id of the entry holding the full output.
 * Fixed here and nowhere else — it is sent to the model on every later
 * request, so it must never change under a stored entry. */
#define TURN_REVIEW_OMIT_FMT "[… %d lines omitted; full output is entry %lld …]"

/* How the review entry's note is introduced to the model (llm_payload.c). */
#define TURN_REVIEW_NOTE_PREFIX "[Turn review] "

typedef enum {
    TURN_REVIEW_APPLIED  = 1,   /* the turn was edited and a review entry written */
    TURN_REVIEW_NOTHING  = 0,   /* the reply asked for no change; nothing written */
    TURN_REVIEW_REJECTED = -1,  /* the reply or the turn failed a check; nothing written */
    TURN_REVIEW_ERROR    = -2,  /* our side failed (DB, memory); nothing written */
} TurnReviewResult;

/* The finished turn as the reviewer's input (JSON text, caller frees): the
 * entries of the newest turn on the session's active branch. NULL when the
 * session's leaf is not an assistant message — there is no finished turn. */
char *turn_review_input(sqlite3 *db, int64_t session_id);

/* Size of the newest turn on the branch, as token estimates: everything in
 * it, and its tool_call + tool_result entries alone. Returns 0, or -1 when
 * the session has no entries. Either pointer may be NULL. */
int turn_review_measure(sqlite3 *db, int64_t session_id, int *turn_tokens,
                        int *tool_tokens);

/* Validate a reviewer's reply against the finished turn and apply it, all or
 * nothing, in one BEGIN IMMEDIATE transaction — so the handle must not have
 * a transaction open. On REJECTED or ERROR *err_out (when given) gets a
 * one-line reason the caller frees; it is NULL otherwise. */
TurnReviewResult turn_review_apply(sqlite3 *db, int64_t session_id,
                                   const char *reply_json,
                                   const char *reviewer_model, char **err_out);

#endif
