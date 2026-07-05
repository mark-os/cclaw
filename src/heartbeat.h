#ifndef CCLAW_HEARTBEAT_H
#define CCLAW_HEARTBEAT_H

#include "types.h"
#include "sqlite3.h"

/* Start heartbeat timer thread. Injects heartbeat user message into
 * inbox for idle sessions every cfg->heartbeat_interval seconds, then signals
 * daemon to fork agent. No-op if interval <= 0.
 * Returns 0 on success (or disabled), -1 on thread creation failure. */
int heartbeat_start(const Config *cfg, sqlite3 *db);

/* Stop heartbeat thread. Safe to call even if not started. */
void heartbeat_stop(void);

#endif
