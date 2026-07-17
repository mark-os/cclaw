#define _GNU_SOURCE
#include "tool_channel_send.h"
#include "tool_args.h"
#include "channel_api.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Does a route for (channel, chat_id) resolve to this agent — directly by
 * agent_name, or through a pinned session owned by the agent? Exact chat_id
 * only: a '*' wildcard routes inbound traffic, it does not hand out
 * channel-wide send authority. */
static int send_target_allowed(sqlite3 *db, const char *agent,
                               const char *channel, const char *chat_id) {
    const char *sql =
        "SELECT 1 FROM channel_routes r"
        " WHERE r.channel_name=?1 AND r.channel_id=?2"
        "   AND (r.agent_name=?3 OR EXISTS (SELECT 1 FROM sessions s"
        "        WHERE s.id=r.session_id AND s.agent_name=?3));";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, chat_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, agent, -1, SQLITE_STATIC);
    int ok = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return ok;
}

/* action=list: reachable targets, straight from channel_routes. */
static char *list_targets(sqlite3 *db, const char *agent) {
    const char *sql =
        "SELECT COALESCE(json_group_array(json_object("
        "  'channel', r.channel_name, 'chat_id', r.channel_id,"
        "  'delivery_mode', r.delivery_mode)), '[]')"
        " FROM channel_routes r"
        " WHERE r.channel_id <> '*'"
        "   AND (r.agent_name=?1 OR EXISTS (SELECT 1 FROM sessions s"
        "        WHERE s.id=r.session_id AND s.agent_name=?1));";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return strdup("error: list failed (db)");
    sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out ? out : strdup("[]");
}

static char *tool_channel_send_handler(const char *arguments, void *user_data) {
    ToolChannelSendCtx *ctx = (ToolChannelSendCtx *)user_data;
    if (!ctx || !ctx->db) return strdup("error: channel_send unavailable");

    char *action = tool_args_str(ctx->db, arguments, "action");

    if (action && strcmp(action, "list") == 0) {
        free(action);
        return list_targets(ctx->db, ctx->agent_name);
    }
    if (action && strcmp(action, "send") != 0) {
        free(action);
        return strdup("error: action must be 'send' or 'list'");
    }
    free(action);

    char *channel = tool_args_str(ctx->db, arguments, "channel");
    char *chat_id = tool_args_str(ctx->db, arguments, "chat_id");
    char *message = tool_args_str(ctx->db, arguments, "message");
    if (!channel || !chat_id || !message || !message[0]) {
        free(channel); free(chat_id); free(message);
        return strdup("error: channel, chat_id, and message are required");
    }

    /* Routes are the allowlist — default-deny. Reaching a new target is a
     * privileged act: an operator `cclaw route add`, never implicit. */
    if (!send_target_allowed(ctx->db, ctx->agent_name, channel, chat_id)) {
        char *m = tool_errf("error: no route for (%s, %s) resolves to you — "
                       "channel_send targets need an operator route "
                       "(cclaw route add). Use action='list' to see yours.",
                       channel, chat_id);
        free(channel); free(chat_id); free(message);
        return m;
    }

    /* Dedup with auto mode: targeting the session's own origin chat when the
     * route already auto-delivers turn output would double-post. */
    {
        const char *dsql =
            "SELECT 1 FROM sessions s"
            " WHERE s.id=?1 AND s.channel_name=?2 AND s.channel_id=?3"
            "   AND COALESCE((SELECT r.delivery_mode FROM channel_routes r"
            "        WHERE r.channel_name=?2 AND r.channel_id IN (?3,'*')"
            "        ORDER BY r.channel_id=?3 DESC LIMIT 1), 'auto') = 'auto';";
        sqlite3_stmt *st;
        int dup = 0;
        if (sqlite3_prepare_v2(ctx->db, dsql, -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, ctx->session_id);
            sqlite3_bind_text(st, 2, channel, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 3, chat_id, -1, SQLITE_STATIC);
            dup = (sqlite3_step(st) == SQLITE_ROW);
            sqlite3_finalize(st);
        }
        if (dup) {
            free(channel); free(chat_id); free(message);
            return strdup("not sent: this chat is your session's origin and the "
                          "route is in auto mode — your reply already delivers "
                          "there.");
        }
    }

    const char *isql =
        "INSERT INTO channel_outbox(channel_name, session_id, payload)"
        " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4));";
    sqlite3_stmt *st;
    int64_t oid = -1;
    if (sqlite3_prepare_v2(ctx->db, isql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, ctx->session_id);
        sqlite3_bind_text(st, 3, chat_id, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 4, message, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_DONE)
            oid = sqlite3_last_insert_rowid(ctx->db);
        sqlite3_finalize(st);
    }
    if (oid < 0) {
        free(channel); free(chat_id); free(message);
        return strdup("error: send failed (db)");
    }
    if (ctx->db_path) channel_outbox_wake(ctx->db_path, channel);
    free(channel); free(chat_id); free(message);
    return tool_errf("queued, outbox id %lld", (long long)oid);
}

static const char *CHANNEL_SEND_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"description\":\"'send' (default) or 'list' reachable targets\"},"
    "\"channel\":{\"type\":\"string\",\"description\":\"Channel name (e.g. telegram)\"},"
    "\"chat_id\":{\"type\":\"string\",\"description\":\"Target chat id — must be routed to you\"},"
    "\"message\":{\"type\":\"string\",\"description\":\"Plain text to send\"}"
    "},\"required\":[]}";

int tool_channel_send_register(ToolRegistry *reg, ToolChannelSendCtx *ctx) {
    int rc = tools_register(reg, "channel_send",
        "Send a message to a chat routed to you (deliberate speech — the only "
        "outbound path for explicit-mode routes, e.g. group chats). Targets "
        "are allowlisted by channel_routes; action='list' shows yours.",
        CHANNEL_SEND_PARAMS, tool_channel_send_handler, ctx);
    tools_set_recipe(reg, "channel_send", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}
