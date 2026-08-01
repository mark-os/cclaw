#ifndef CCLAW_ADVANCE_H
#define CCLAW_ADVANCE_H

/* The session advancement state machine: advance_session() reads a
 * session's state + leaf entry and returns the single next action (dispatch
 * an LLM call, dispatch tools, deliver, wait). The load-bearing wall — every
 * turn moves through here.
 */

#include "db.h"
#include "types.h"
#include <stdint.h>

/* Return values from advance_session — tells caller what to do next */
typedef enum {
    ADVANCE_NOOP = 0,        /* nothing to do (already idle, no inbox) */
    ADVANCE_DISPATCH_LLM,    /* dispatch LLM request for this session */
    ADVANCE_DISPATCH_TOOLS,  /* dispatch pending tool calls */
    ADVANCE_DONE,            /* turn complete — deliver response */
    ADVANCE_WAITING,         /* parked for blocking sub-agent */
    ADVANCE_ERROR            /* unrecoverable error */
} AdvanceResult;

/* Filled by advance_session for the caller to act on */
typedef struct {
    AdvanceResult action;
    int64_t session_id;
    char agent_name[64];
    int iteration;           /* current iteration for LLM dispatch */
    int tc_count;            /* number of pending tool calls */
    PendingToolCall *calls;  /* caller must free with db_tool_call_free_pending */
} AdvanceOutput;

/* Single idempotent session transition function.
 * Reads DB state, decides next action, does lightweight state updates.
 * Does NOT fork processes — caller acts on the returned AdvanceOutput.
 * max_iterations: cap on turn iterations (0 = 25 default). */
AdvanceOutput advance_session(sqlite3 *db, int64_t session_id, int max_iterations);

/* Tell a sub-agent's parent that the child finished: blocking mode writes the
 * ToolResult for the parent's launch_agent call, background mode posts to the
 * parent inbox. Stamps sessions.parent_notified_at on the child inside the same
 * transaction — a lost push leaves NULL, which is what the sweep below finds.
 * No-op for a session without a parent. */
void advance_notify_parent(sqlite3 *db, int64_t child_session_id, int is_error);

/* The convergence sweep: re-notify every terminal child whose push was lost
 * (state='idle', has a parent, parent_notified_at IS NULL, assistant leaf).
 * Idempotent by the stamp. Returns the number re-notified. Called from the
 * daemon's periodic tick — pushes are latency, this is the guarantee. */
int advance_sweep_unnotified(sqlite3 *db);

#endif
