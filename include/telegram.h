#ifndef CCLAW_TELEGRAM_H
#define CCLAW_TELEGRAM_H

#include "types.h"
#include "sqlite3.h"
#include <stddef.h>

/* Start Telegram getUpdates poller thread.
 * Requires cfg->telegram_token to be set.
 * Returns 0 on success, -1 on error. */
int telegram_start(const Config *cfg, sqlite3 *db);

/* Signal poller thread to stop and join. */
void telegram_stop(void);

/* V11: Find split point within text[0..len-1], respecting max_len.
 * Splits at paragraph, then newline, then sentence, then hard cut.
 * Exposed for testing. */
size_t tg_find_split(const char *text, size_t len, size_t max_len);

/* V2: Compute backoff delay in seconds given consecutive failure count.
 * Doubles each failure (1, 2, 4, 8, ...), capped at 60s. Exposed for testing. */
int tg_backoff_delay(int consecutive_failures);

#endif
