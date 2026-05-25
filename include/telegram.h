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

/* V11: Send message to chat_id (chunked at 4096 chars).
 * Used by daemon for response delivery (T85). */
void telegram_send_message(const char *token, int64_t chat_id, const char *text);

/* V53: Check if chat_id is in admin_chat_ids[]. */
int telegram_is_admin(const Config *cfg, int64_t chat_id);

/* T140: Parse admin command text. Returns command type (1=key, 2=config, 3=whitelist, 0=unknown).
 * If args_out is non-NULL, sets it to the argument string after the command (or NULL if none). */
int telegram_parse_admin_command(const char *text, const char **args_out);

/* V11: Find split point within text[0..len-1], respecting max_len.
 * Splits at paragraph, then newline, then sentence, then hard cut.
 * Exposed for testing. */
size_t tg_find_split(const char *text, size_t len, size_t max_len);

/* V2: Compute backoff delay in seconds given consecutive failure count.
 * Doubles each failure (1, 2, 4, 8, ...), capped at 60s. Exposed for testing. */
int tg_backoff_delay(int consecutive_failures);

#endif
