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
    int deferred;            /* NOOP because session_max_concurrent is reached —
                              * retry when a resource frees (caller may note the
                              * session for an edge wake; the inbox sweep is the
                              * backstop either way) */
} AdvanceOutput;

/* Single idempotent session transition function.
 * Reads DB state, decides next action, does lightweight state updates.
 * Does NOT fork processes — caller acts on the returned AdvanceOutput.
 * max_iterations: cap on turn iterations (0 = 25 default). */
AdvanceOutput advance_session(sqlite3 *db, int64_t session_id, int max_iterations);

/* Evaluate every delivery edge of a session at a turn boundary
 * (specs/delivery.md): the one-shot blocking reply edge first (its firing
 * advances the standing cursor in the same transaction), then standing
 * parent/channel edges per their policy. is_error marks an error boundary —
 * the "failed" label, and it bypasses the quiescence hold so parked parents
 * never hang on a subtree that can't settle. include_channel=0 suppresses
 * channel edges (LLM-error turns never ship to chat) while still recording
 * the boundary in their cursor. Idempotent by the cursors. Returns payloads
 * shipped, -1 on a transaction failure (the sweep retries). */
int advance_deliver_boundary(sqlite3 *db, int64_t session_id, int is_error,
                             int include_channel);

/* Child outcome tag input (F2): 1 when the session's newest assistant entry
 * stopped at the model's output cap. Delivered results are tagged
 * completed | failed | truncated from state alone — never from prose. */
int session_final_truncated(sqlite3 *db, int64_t session_id);

/* Mid-turn hook for 'iteration' edges: ship content-bearing assistant entries
 * that landed since each edge's cursor. Called after an LLM completion whose
 * turn continues (pending tool calls); the boundary evaluation covers the
 * final message. No-op without an iteration edge. */
void advance_deliver_iteration(sqlite3 *db, int64_t session_id);

/* The convergence sweep, generalized to the cursor-lag predicate: any
 * non-explicit edge whose cursor is behind an idle session's assistant leaf
 * is re-evaluated — lost pushes re-derive, quiescent holds re-check their
 * subtree, unfired one-shots fire. Idempotent by the cursors. Returns
 * payloads shipped. Called from the daemon's periodic tick — pushes are
 * latency, this is the guarantee. */
int advance_sweep_undelivered(sqlite3 *db);

#endif
