#ifndef CCLAW_JANITOR_H
#define CCLAW_JANITOR_H

#include "sqlite3.h"

/* V16,V19: Sweep stale locks and orphan pending sessions.
 * Returns number of sessions recovered, or -1 on error. */
int janitor_sweep(sqlite3 *db, int stale_timeout_sec);

#endif
