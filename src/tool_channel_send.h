#ifndef CCLAW_TOOL_CHANNEL_SEND_H
#define CCLAW_TOOL_CHANNEL_SEND_H

#include "tools.h"
#include "sqlite3.h"

/* channel_send — deliberate outbound message to a routed chat (the reverse
 * of the inbound gate; specs/channels.md). Routes are the allowlist: an
 * agent may target only (channel, chat_id) pairs whose route resolves to it.
 * Ships ungranted — the operator grants it per agent. */
typedef struct {
    sqlite3 *db;
    const char *db_path;      /* for channel_outbox_wake */
    int64_t session_id;       /* threaded per call by dispatch */
    char agent_name[64];      /* threaded per call by dispatch */
} ToolChannelSendCtx;

int tool_channel_send_register(ToolRegistry *reg, ToolChannelSendCtx *ctx);

#endif
