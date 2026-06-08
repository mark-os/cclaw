#ifndef CCLAW_CHANNEL_API_H
#define CCLAW_CHANNEL_API_H

#include "sqlite3.h"
#include <stdint.h>

/* T243/V99: Channel API — limited interface for channel processes.
 * Channel processes use this to communicate with daemon via cclaw.db. */

typedef struct {
    sqlite3 *db;          /* cclaw.db handle */
    char *channel_name;   /* this channel's name */
    char *db_path;        /* path to cclaw.db (for daemon_wake FIFO) */
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

/* V100: Insert channel_events row + daemon_wake(). */
int channel_emit(ChannelCtx *ctx, const char *event_type, const char *payload);

/* V102: Read from channel_state kv. Caller frees returned string. */
char *channel_get_config(ChannelCtx *ctx, const char *key);

/* V102: Write to channel_state kv. */
int channel_set_config(ChannelCtx *ctx, const char *key, const char *value);

/* V101: Get oldest pending outbox row for this channel. Caller frees via channel_outbox_row_free. */
ChannelOutboxRow *channel_next_outbox(ChannelCtx *ctx);
void channel_outbox_row_free(ChannelOutboxRow *row);

/* V101: Mark outbox row as delivered. */
int channel_ack_outbox(ChannelCtx *ctx, int64_t id);

/* V101: Mark outbox row as failed. */
int channel_fail_outbox(ChannelCtx *ctx, int64_t id, const char *error);

/* V105: Wake daemon via named FIFO (1 byte write). */
int daemon_wake(const char *db_path);

/* Per-channel outbox wake FIFO (daemon → channel process). */
char *channel_outbox_fifo_path(const char *db_path, const char *channel_name);
int channel_outbox_fifo_open(const char *db_path, const char *channel_name);
void channel_outbox_fifo_close(int fd, const char *db_path, const char *channel_name);
int channel_outbox_wake(const char *db_path, const char *channel_name);

/* Insert outbox row for a channel. Called by daemon after agent turn completes. */
int channel_outbox_insert(sqlite3 *db, const char *channel_name,
                          int64_t session_id, const char *payload);

#endif
