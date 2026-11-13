#ifndef CCLAW_TOOL_SHARE_H
#define CCLAW_TOOL_SHARE_H

#include "tools.h"
#include "tool_channel_send.h"

/* share_file — put one workspace file behind a link on the daemon's web
 * server (/share/<token>/<name>, web.c) for a limited time. For what a chat
 * attachment cannot carry: a file too large for the platform, a channel that
 * takes no files, a link to hand on. The token is the capability; the row in
 * `shares` is the share (nothing is copied).
 *
 * Shares ToolChannelSendCtx: both tools need exactly the per-call identity
 * dispatch already threads into it (agent, session, workspace). Ships
 * ungranted — publishing a file over HTTP is the operator's call per agent. */
int tool_share_register(ToolRegistry *reg, ToolChannelSendCtx *ctx);

#endif
