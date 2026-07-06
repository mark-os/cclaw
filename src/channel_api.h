#ifndef CCLAW_CHANNEL_API_H
#define CCLAW_CHANNEL_API_H

#include "sqlite3.h"
#include <stdint.h>

/* Channel API — limited interface for channel processes.
 * Channel processes use this to communicate with daemon via cclaw.db. */

typedef struct {
    sqlite3 *db;          /* cclaw.db handle */
    char *channel_name;   /* this channel's name */
    char *db_path;        /* path to cclaw.db (for the daemon wake FIFO) */
} ChannelCtx;

/* Outbox row returned by channel_next_outbox */
typedef struct {
    int64_t id;
    int64_t session_id;
    char *payload;
} ChannelOutboxRow;

/* Create context. Caller owns returned ctx (free with channel_ctx_free). */
ChannelCtx *channel_ctx_open(const char *db_path, const char *channel_name);
void channel_ctx_free(ChannelCtx *ctx);

/* Insert channel_events row + wake the daemon via the FIFO. */
int channel_emit(ChannelCtx *ctx, const char *event_type, const char *payload,
                 const char *external_id);

/* Read from channel_state kv. Caller frees returned string. */
char *channel_get_config(ChannelCtx *ctx, const char *key);

/* Write to channel_state kv. */
int channel_set_config(ChannelCtx *ctx, const char *key, const char *value);

/* Get oldest pending outbox row for this channel. Caller frees via channel_outbox_row_free. */
ChannelOutboxRow *channel_next_outbox(ChannelCtx *ctx);
void channel_outbox_row_free(ChannelOutboxRow *row);

/* Mark a pending row 'sending' once handed to JS (async send in flight). */
int channel_dispatch_outbox(ChannelCtx *ctx, int64_t id);

/* Startup recovery: 'sending' rows (crashed mid-send) back to 'pending'. */
int channel_reset_outbox(ChannelCtx *ctx);

/* Mark outbox row as delivered. */
int channel_ack_outbox(ChannelCtx *ctx, int64_t id);

/* Mark outbox row as failed. */
int channel_fail_outbox(ChannelCtx *ctx, int64_t id, const char *error);

/* Reschedule a failed send for retry: bump attempt count, return to
 * pending with a future next_attempt_at. Non-blocking backoff. */
int channel_retry_outbox(ChannelCtx *ctx, int64_t id, int delay_sec);

/* Current retry attempt count for an outbox row, or 0 if unknown. */
int channel_outbox_attempts(ChannelCtx *ctx, int64_t id);

/* Per-channel request UDS path (daemon proxies inbound HTTP here).
 * Format: <db_path_without_.db>.<channel_name>.sock — caller frees. */
char *channel_uds_path(const char *db_path, const char *channel_name);

/* Per-channel outbox wake FIFO (daemon → channel process). */
char *channel_outbox_fifo_path(const char *db_path, const char *channel_name);
int channel_outbox_fifo_open(const char *db_path, const char *channel_name);
void channel_outbox_fifo_close(int fd, const char *db_path, const char *channel_name);
int channel_outbox_wake(const char *db_path, const char *channel_name);

#endif
