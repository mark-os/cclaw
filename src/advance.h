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

/* ── Cross-turn runaway guard: the classification, in one place ────
 * The incident class is turns generating turns with no human in the causal
 * chain (cron chatter, sub-agent respawn ping-pong). The metric is the
 * consecutive-autonomous-turn streak, derived by SQL at turn open — see
 * advance.c for the gate and specs/schema.md for the contract.
 *
 * The list is *autonomous*-side because the human side is open-ended: 'cli',
 * plus any channel name (channels are created at runtime, so no fixed list
 * could enumerate them), plus 'approval' — a post-window approval notice
 * normally carries a human decision, and an expiry is rare and non-recurring,
 * so neither can sustain a loop. A role-1 entry with no data stamp is human
 * too: the direct-append path (CLI) writes none, and every autonomous writer
 * goes through the inbox drain, which always stamps.
 *
 * SID is the SQL expression naming the session ("?1" from a bound statement,
 * "s.id" from a correlated subquery). */
#define CCLAW_AUTONOMOUS_SOURCES \
    "('cron','cron_error','agent_result','spawn','system')"

/* The most recent turn a human opened; 0 = never, so every turn counts. */
#define CCLAW_SQL_LAST_HUMAN_TURN(SID) \
    "COALESCE((SELECT MAX(h.turn_id) FROM entries h" \
    "  WHERE h.session_id=" SID " AND h.role=1" \
    "    AND COALESCE(json_extract(h.data,'$.source'),'') NOT IN " \
             CCLAW_AUTONOMOUS_SOURCES "), 0)"

/* Consecutive autonomous turns already on record for this session. */
#define CCLAW_SQL_AUTONOMOUS_STREAK(SID) \
    "(SELECT COUNT(DISTINCT t.turn_id) FROM entries t" \
    "  WHERE t.session_id=" SID \
    "    AND t.turn_id > " CCLAW_SQL_LAST_HUMAN_TURN(SID) ")"

/* Is there queued work, and is every queued row autonomous? The whole pending
 * set, not just the batch the drain would take: a human row anywhere in the
 * queue means this session is not running away. */
#define CCLAW_SQL_INBOX_ALL_AUTONOMOUS(SID) \
    "(EXISTS(SELECT 1 FROM inbox q WHERE q.session_id=" SID " AND q.consumed=0)" \
    " AND NOT EXISTS(SELECT 1 FROM inbox q WHERE q.session_id=" SID \
    "   AND q.consumed=0" \
    "   AND COALESCE(q.source,'') NOT IN " CCLAW_AUTONOMOUS_SOURCES "))"

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
