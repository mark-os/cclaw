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

/* Turn-final delivery: CLI stdout for the root session, or the daemon's FIFO
 * nudge for whatever advance_deliver_boundary already wrote to the outbox. */
void deliver_response(int64_t session_id);

/* ── INTERIM: main.c-resident callees of the above ──────────────────
 * These are still statics-turned-externs in main.c; loop.o carries undefined
 * references to them that only resolve at the final cclaw link. Each moves to
 * a real module later in PROJECT:main-c-reorg — when it does, delete its
 * declaration here and include that module's header instead. Nothing else
 * should declare or call these.
 */
int  dispatch_tool(int64_t session_id, const char *agent_name,
                   PendingToolCall *tc);            /* → dispatch.c (step 6) */
int  dispatch_llm_req(int64_t session_id, const char *agent_name,
                      int iteration);               /* → dispatch.c (step 6) */
void stalled_add(int64_t session_id);               /* → dispatch.c (step 6) */
void approval_flush_deferred(void);                 /* → approval.c (step 8) */

#endif
