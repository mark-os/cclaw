#ifndef CCLAW_TELEGRAM_H
#define CCLAW_TELEGRAM_H

#include "types.h"
#include "sqlite3.h"

/* Start Telegram getUpdates poller thread.
 * Requires cfg->telegram_token to be set.
 * Returns 0 on success, -1 on error. */
int telegram_start(const Config *cfg, sqlite3 *db);

/* Signal poller thread to stop and join. */
void telegram_stop(void);

#endif
