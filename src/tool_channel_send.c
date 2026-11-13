#define _GNU_SOURCE
#include "tool_channel_send.h"
#include "tool_args.h"
#include "channel_api.h"
#include "config_registry.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Does the chat's route resolve to this agent? One rule: the pinned
 * session's agent (routes never name an agent — channels.default_agent
 * routes inbound traffic only, it hands out no send authority). */
static int send_target_allowed(sqlite3 *db, const char *agent,
                               const char *channel, const char *chat_id) {
    const char *sql =
        "SELECT 1 FROM channel_routes r"
        " JOIN sessions s ON s.id = r.session_id"
        " WHERE r.channel_name=?1 AND r.chat_id=?2 AND s.agent_name=?3;";
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
static char *list_targets(sqlite3 *db, const char *agent, int *is_error) {
    const char *sql =
        "SELECT COALESCE(json_group_array(json_object("
        "  'channel', r.channel_name, 'chat_id', r.chat_id,"
        /* 'name' is the chat's observed display title (may be NULL until a
         * runner reports one) — for the model to pick a target by; the send
         * still goes by chat_id. */
        "  'name', s.chat_title,"
        "  'delivery_mode', r.delivery_mode)), '[]')"
        " FROM channel_routes r"
        " JOIN sessions s ON s.id = r.session_id"
        " WHERE s.agent_name=?1;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return tool_fail(is_error, "error: list failed (db)");
    sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out ? out : strdup("[]");
}

/* An attachment, copied out of the agent's workspace into the channel's
 * media spool. */
typedef struct {
    char path[1200];     /* the spool copy */
    char name[80];       /* what the recipient sees */
    const char *mime;
} SendFile;

/* Copy the agent's file into the channel's spool. This tool runs in the
 * trusted parent, outside any sandbox, and the path comes from the model, so
 * the file is opened with util_open_inside (the check is on the opened file,
 * not the path). The spool then holds a copy the agent can no longer change.
 * Returns NULL on success, else a malloc'd error for the model. */
static char *spool_file(ToolChannelSendCtx *ctx, const char *channel,
                        const char *file, SendFile *out) {
    char *err = NULL;
    char real[PATH_MAX], dir[1024];
    int fd = util_open_inside(ctx->workspace, file, real, sizeof(real));
    if (fd < 0) {
        int rc = errno == EXDEV
            ? asprintf(&err, "error: '%s' is outside your workspace — only "
                       "files in your workspace can be sent", file)
            : asprintf(&err, "error: cannot open file '%s': %s", file,
                       strerror(errno));
        return rc < 0 ? strdup("error: cannot open the file") : err;
    }
    struct stat st;
    long long max = config_get_int(ctx->db, "channel_send_file_max_bytes");
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (asprintf(&err, "error: '%s' is not a regular file", file) < 0) err = NULL;
    } else if (st.st_size == 0) {
        if (asprintf(&err, "error: '%s' is empty", file) < 0) err = NULL;
    } else if (max > 0 && st.st_size > max) {
        if (asprintf(&err, "error: '%s' is %lld bytes; the limit is %lld "
                     "(config channel_send_file_max_bytes)", file,
                     (long long)st.st_size, max) < 0) err = NULL;
    } else if (channel_media_dir(ctx->db_path, channel, dir, sizeof(dir)) != 0) {
        if (asprintf(&err, "error: no media spool for channel %s", channel) < 0) err = NULL;
    }
    if (err) { close(fd); return err; }

    /* Recipient-visible name: the basename, reduced to characters that are
     * safe in a multipart header and in a path. */
    const char *base = strrchr(real, '/');
    base = base ? base + 1 : real;
    size_t k = 0;
    for (; base[k] && k < sizeof(out->name) - 1; k++) {
        unsigned char ch = (unsigned char)base[k];
        out->name[k] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' ||
                        ch == '_') ? (char)ch : '_';
    }
    out->name[k] = '\0';
    if (out->name[0] == '.') out->name[0] = '_';

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(out->path, sizeof(out->path), "%s/out-%lld-%lld%09ld-%s", dir,
             (long long)ctx->session_id, (long long)ts.tv_sec, ts.tv_nsec, out->name);
    int ofd = open(out->path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    unsigned char buf[65536], head[12] = {0};
    long long copied = 0;
    ssize_t r = 0;
    while (ofd >= 0 && (r = read(fd, buf, sizeof(buf))) > 0) {
        if (copied == 0) memcpy(head, buf, (size_t)r < sizeof(head) ? (size_t)r : sizeof(head));
        copied += r;
        /* the file may still be growing; the limit is on what is sent */
        if ((max > 0 && copied > max) || write(ofd, buf, (size_t)r) != r) { r = -1; break; }
    }
    close(fd);
    if (ofd < 0 || r < 0 || close(ofd) != 0 || copied == 0) {
        if (ofd >= 0) unlink(out->path);
        if (asprintf(&err, "error: could not copy '%s' for sending", file) < 0) err = NULL;
        return err ? err : strdup("error: could not copy the file for sending");
    }
    /* What the platform should show it as: an image previews inline only
     * if the platform is told it is one. */
    out->mime = util_sniff_mime(head, copied < (long long)sizeof(head)
                                          ? (size_t)copied : sizeof(head));
    if (!out->mime) out->mime = "application/octet-stream";
    return NULL;
}

static char *tool_channel_send_handler(const char *arguments, void *user_data, int *is_error) {
    ToolChannelSendCtx *ctx = (ToolChannelSendCtx *)user_data;
    if (!ctx || !ctx->db) return tool_fail(is_error, "error: channel_send unavailable");

    char *action = tool_args_str(ctx->db, arguments, "action");

    if (action && strcmp(action, "list") == 0) {
        free(action);
        return list_targets(ctx->db, ctx->agent_name, is_error);
    }
    if (action && strcmp(action, "send") != 0) {
        free(action);
        return tool_fail(is_error, "error: action must be 'send' or 'list'");
    }
    free(action);

    char *channel = tool_args_str(ctx->db, arguments, "channel");
    char *chat_id = tool_args_str(ctx->db, arguments, "chat_id");
    char *message = tool_args_str(ctx->db, arguments, "message");
    char *file = tool_args_str(ctx->db, arguments, "file");
    if (file && !file[0]) { free(file); file = NULL; }

    /* No target named → the session's own bound chat. Models don't reliably
     * know their chat's snowflake (and hallucinate one when pressed); the
     * common case — speak into the chat you're listening to — needs no id. */
    if ((!channel || !chat_id) && ctx->session_id > 0) {
        const char *bsql =
            "SELECT channel_name, chat_id FROM sessions"
            " WHERE id=?1 AND channel_name IS NOT NULL AND chat_id IS NOT NULL;";
        sqlite3_stmt *bs;
        if (sqlite3_prepare_v2(ctx->db, bsql, -1, &bs, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(bs, 1, ctx->session_id);
            if (sqlite3_step(bs) == SQLITE_ROW) {
                if (!channel) {
                    const char *v = (const char *)sqlite3_column_text(bs, 0);
                    if (v) channel = strdup(v);
                }
                if (!chat_id) {
                    const char *v = (const char *)sqlite3_column_text(bs, 1);
                    if (v) chat_id = strdup(v);
                }
            }
            sqlite3_finalize(bs);
        }
    }
    if (!channel || !chat_id || ((!message || !message[0]) && !file)) {
        free(channel); free(chat_id); free(message); free(file);
        return tool_fail(is_error, "error: message or file is required; channel+chat_id only "
                      "default to your own chat when the session is bound "
                      "to one (use action='list' for other targets)");
    }

    /* Routes are the allowlist — default-deny. Reaching a new target is a
     * privileged act: an operator `cclaw route add`, never implicit. */
    if (!send_target_allowed(ctx->db, ctx->agent_name, channel, chat_id)) {
        char *m = tool_fail(is_error, "error: no route for (%s, %s) resolves to you — "
                       "channel_send targets need an operator route "
                       "(cclaw route add). Use action='list' to see yours.",
                       channel, chat_id);
        free(channel); free(chat_id); free(message); free(file);
        return m;
    }

    /* Dedup with auto mode: targeting the session's own origin chat when the
     * route already auto-delivers turn output would double-post. A file is
     * not part of that output — this tool is the only way one is sent — so
     * the rule is about text alone. */
    if (!file) {
        const char *dsql =
            "SELECT 1 FROM sessions s"
            " WHERE s.id=?1 AND s.channel_name=?2 AND s.chat_id=?3"
            "   AND COALESCE((SELECT r.delivery_mode FROM channel_routes r"
            "        WHERE r.channel_name=?2 AND r.chat_id=?3), 'auto') = 'auto';";
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

    SendFile sf = {0};
    if (file) {
        char *ferr = spool_file(ctx, channel, file, &sf);
        if (ferr) {
            free(channel); free(chat_id); free(message); free(file);
            *is_error = 1;
            return ferr;
        }
    }

    /* A file row has no 'text': the message rides as 'caption'. A channel
     * handler that predates files then fails the row ("missing chat_id or
     * text") and the agent is told, instead of the text arriving alone and
     * the file being dropped without a word. */
    const char *isql = file
        ? "INSERT INTO channel_outbox(channel_name, session_id, payload)"
          " VALUES(?1, ?2, json_object('chat_id', ?3, 'caption', ?4,"
          "   'file', json_object('path', ?5, 'name', ?6, 'mime', ?7)));"
        : "INSERT INTO channel_outbox(channel_name, session_id, payload)"
          " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4));";
    sqlite3_stmt *st;
    int64_t oid = -1;
    if (sqlite3_prepare_v2(ctx->db, isql, -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, channel, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, ctx->session_id);
        sqlite3_bind_text(st, 3, chat_id, -1, SQLITE_STATIC);
        if (message && message[0])
            sqlite3_bind_text(st, 4, message, -1, SQLITE_STATIC);
        if (file) {
            sqlite3_bind_text(st, 5, sf.path, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 6, sf.name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 7, sf.mime, -1, SQLITE_STATIC);
        }
        if (sqlite3_step(st) == SQLITE_DONE)
            oid = sqlite3_last_insert_rowid(ctx->db);
        sqlite3_finalize(st);
    }
    if (oid < 0) {
        if (file) unlink(sf.path);
        free(channel); free(chat_id); free(message); free(file);
        return tool_fail(is_error, "error: send failed (db)");
    }
    if (ctx->db_path) channel_outbox_wake(ctx->db_path, channel);
    free(channel); free(chat_id); free(message);
    if (file) {
        free(file);
        return tool_errf("queued, outbox id %lld (%s, %s) — you are told if "
                         "the channel cannot deliver it",
                         (long long)oid, sf.name, sf.mime);
    }
    return tool_errf("queued, outbox id %lld", (long long)oid);
}

static const char *CHANNEL_SEND_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"description\":\"'send' (default) or 'list' reachable targets (each with its chat name)\"},"
    "\"channel\":{\"type\":\"string\",\"description\":\"Channel name (e.g. telegram); omit to target your own chat\"},"
    "\"chat_id\":{\"type\":\"string\",\"description\":\"Target chat id — must be routed to you; omit to target your own chat. Never guess an id: omit it or take one from action='list'\"},"
    "\"message\":{\"type\":\"string\",\"description\":\"Plain text to send (with file: its caption)\"},"
    "\"file\":{\"type\":\"string\",\"description\":\"A file in your workspace to attach (an image shows inline) — the way to show the user a picture or hand them a document\"}"
    "},\"required\":[]}";

int tool_channel_send_register(ToolRegistry *reg, ToolChannelSendCtx *ctx) {
    int rc = tools_register(reg, "channel_send",
        "Send a message to a chat routed to you (deliberate speech — the only "
        "outbound path for explicit-mode routes, e.g. group chats). Targets "
        "are allowlisted by channel_routes; action='list' shows yours. "
        "file attaches a file from your workspace, to any routed chat "
        "including your own.",
        CHANNEL_SEND_PARAMS, tool_channel_send_handler, ctx);
    tools_set_recipe(reg, "channel_send", (ToolRecipe){.vehicle = EXEC_INLINE});
    return rc;
}
