#ifndef CCLAW_SETTLE_H
#define CCLAW_SETTLE_H

/* The end of an iteration, the executor side of script hooks: start the
 * agent's hooks for it and — for a hook that is waited for — hold the session
 * in state 'settling' until the script is gone and its reply has been through
 * turn_edit_apply. As loop.c is of advance.c; see specs/hooks.md.
 *
 * The order (plan: session hooks and compaction, "Order at the end of each
 * iteration"), one hold for all of it:
 *
 *   1. the iteration ended the turn with an answer: Stop (SubagentStop in a
 *      sub-agent's session) hooks, one at a time;
 *   2. the session is over its trim threshold: ContextThreshold hooks, one
 *      at a time, each only while it is still over;
 *   3. the C check: still over, C trims to prune_target;
 *   4. the next request (mid-turn) or idle (turn end).
 *
 * CacheExpiring is not part of that order. It fires from the timer in the
 * database (cache_expiring.h) on an idle session above its prune target, is
 * held the same way, and is followed by nothing: no C check, no trim
 * (decision 23). A message arriving meanwhile waits for it, as for any hold.
 *
 * Hooks nobody waits for ("async") start with their step, all at once, and
 * cannot change the session. A failure leaves the session as it was and the
 * order goes on.
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

/* A turn has just ended on session_id (idle) and been delivered: steps 1
 * and 2. Returns 1 when the session is now held ('settling') for an awaited
 * hook — the caller skips the C check, which runs when the hold ends — and 0
 * when nothing holds it: the caller runs the C check (turn_end_trim). */
int turn_settle_begin(int64_t session_id, const char *agent);

/* An iteration has ended mid-turn (tool_running, counted; advance_session
 * said ADVANCE_RUN_HOOKS): step 2. Returns 1 when the session is now held;
 * 0 when not — the caller goes on with run_iteration_end. */
int iteration_settle_begin(int64_t session_id, const char *agent);

/* The cache of idle session_id is about to lapse (the timer claimed it):
 * when the agent has CacheExpiring hooks and the session is above its prune
 * target, start them. Returns 1 when the session is now held. */
int cache_settle_begin(int64_t session_id, const char *agent);

/* The timer: claim the sessions due at this moment (cache_expiring_take) and
 * start their CacheExpiring hooks. Daemon event loop, every turn of it. */
void cache_expiring_tick(void);

/* A hook child ended. Applies an awaited hook's reply, counts the outcome
 * (the third failure in a row disables the hook and tells the default
 * agent), then starts the next awaited hook in the order or ends the hold. */
void turn_settle_child_done(const SettleChild *sc);

/* Backstop for a hold whose child is gone without this module hearing of it:
 * every session this process holds in 'settling' with no hook child left is
 * released. Part of the periodic tick. */
void turn_settle_sweep(void);

#endif
