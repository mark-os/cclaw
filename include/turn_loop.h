#ifndef CCLAW_TURN_LOOP_H
#define CCLAW_TURN_LOOP_H

#include "config.h"
#include "agent_setup.h"
#include <sqlite3.h>

/* Turn loop modes */
#define TURN_MODE_CLI    0  /* inherit stdout, prompt user for approvals */
#define TURN_MODE_DAEMON 1  /* pipe stdout, park session for approvals */

/* Turn loop return codes */
#define TURN_EXIT_DONE   0  /* LLM finished (stop) */
#define TURN_EXIT_ERROR  1  /* unrecoverable error */
#define TURN_EXIT_PARKED 2  /* session parked — awaiting external approval */

/* Run the unified turn loop.
 * Forks LLM child, reads tool_calls from DB, dispatches tools, repeats.
 * Returns TURN_EXIT_* code. */
int turn_loop_run(sqlite3 *db, int64_t session_id, const Config *cfg,
                  AgentSetup *setup, int mode);

#endif
