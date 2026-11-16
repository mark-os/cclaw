#ifndef CCLAW_SETTLE_H
#define CCLAW_SETTLE_H

/* The turnSettled event: when a turn has ended and its reply is out, run the
 * agent's turnSettled hooks, and — for a hook that is waited for — hold the
 * session in state 'settling' until the script is gone and its reply has been
 * through turn_edit_apply. The executor side of script hooks, as loop.c is
 * of advance.c; see specs/hooks.md.
 *
 * Event-loop thread only: everything here reads the child table.
 */

#include <stdint.h>

#include "hook.h"

/* A hook child that is gone, as dispatch.c reports it. */
typedef struct {
    int64_t session_id;
    char agent[64];
    HookRef hook;
    int failed;         /* timed out, crashed, or the script reported an error */
    char why[224];      /* for the log, when failed */
    char *reply;        /* what the script wrote, when it did not fail; the
                         * reporter owns and frees it */
} SettleChild;

/* A turn has just ended normally on session_id and been delivered. Start the
 * agent's turnSettled hooks if the turn qualifies. Returns 1 when the session
 * is now held ('settling') for an awaited hook — the caller skips the
 * compaction check, which runs when the hold ends — and 0 when nothing holds
 * it: no hook, turn out of range, or nothing could be started. */
int turn_settle_begin(int64_t session_id, const char *agent);

/* A hook child ended. Applies an awaited hook's reply, counts the outcome,
 * then starts the agent's next awaited hook or ends the hold. */
void turn_settle_child_done(const SettleChild *sc);

/* Backstop for a hold whose child is gone without this module hearing of it:
 * every session this process holds in 'settling' with no hook child left is
 * released. Part of the periodic tick. */
void turn_settle_sweep(void);

#endif
