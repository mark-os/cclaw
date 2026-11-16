#ifndef CCLAW_LOOP_H
#define CCLAW_LOOP_H

/* The executor half of the turn engine: advance.c *decides* what a session
 * should do next, run_advance() *does* it — dispatch an LLM request, claim and
 * dispatch a batch of tool calls, deliver, or release the CLI prompt. Keeping
 * read/decide (advance.c) and act (here) in separate TUs keeps the split
 * legible; the poll loop itself stays in main.c.
 *
 * Lib-resident on purpose: archive modules call run_advance on every
 * completion path, so it cannot live in main.o (which the Makefile filters out
 * of libcclaw.a).
 */

#include <stdint.h>

#include "db.h"

/* Effective iteration cap for a session: agents.max_iterations (if > 0)
 * overrides global config. */
int session_max_iter(int64_t session_id);

/* Advance one session by one decision, executing whatever advance_session
 * returns. Self-recursive: an all-inline tool batch re-advances immediately. */
void run_advance(int64_t session_id);

/* The compaction check that closes a turn: if the idle session needs
 * compacting, mark it 'compacting' and hand it to the worker. Returns 1 when
 * a compaction is now in flight (its completion drains the inbox), 0 when
 * the session was left idle. */
int turn_end_compact(int64_t session_id, const char *agent_name);

/* Turn-final delivery: CLI stdout for the root session, or the daemon's FIFO
 * nudge for whatever advance_deliver_boundary already wrote to the outbox. */
void deliver_response(int64_t session_id);

#endif
