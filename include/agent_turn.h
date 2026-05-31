#ifndef CCLAW_AGENT_TURN_H
#define CCLAW_AGENT_TURN_H

#include <stdint.h>

/* Run a single agent turn on the given session, then exit.
 * This is the daemon's fork+exec target (--agent mode).
 * Config and DB path come from environment variables. */
int agent_turn_run(int64_t session_id);

#endif
