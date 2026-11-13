#define _GNU_SOURCE
#include "channel_api.h"
#include "channel.h"
#include "wake.h"
#include "db.h"

#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── Channel API library ─────────────────────────────────────────── */

ChannelCtx *channel_ctx_open(const char *db_path, const char *channel_name) {
    if (!db_path || !channel_name) return NULL;
    sqlite3 *db = db_open(db_path);
    if (!db) return NULL;
    ChannelCtx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) { db_close(db); return NULL; }
    ctx->db = db;
    ctx->channel_name = strdup(channel_name);
    ctx->db_path = strdup(db_path);
    return ctx;
}

void channel_ctx_free(ChannelCtx *ctx) {
    if (!ctx) return;
    if (ctx->db) db_close(ctx->db);
    free(ctx->channel_name);
    free(ctx->db_path);
    free(ctx);
}

/* Insert event + wake daemon */
int channel_emit(ChannelCtx *ctx, const char *event_type, const char *payload,
                 const char *external_id) {
    if (!ctx || !event_type || !payload) return -1;
    const char *sql =
        "INSERT OR IGNORE INTO channel_events(channel_name, event_type, payload, external_id)"
        " VALUES(?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, ctx->channel_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, event_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, payload, -1, SQLITE_STATIC);
    if (external_id)
        sqlite3_bind_text(stmt, 4, external_id, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    wake_external(ctx->db_path);
    return 0;
}

/* Read channel_state */
char *channel_get_config(ChannelCtx *ctx, const char *key) {
    if (!ctx || !key) return NULL;
    const char *sql =
        "SELECT value FROM channel_state WHERE channel_name=? AND key=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, ctx->channel_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(stmt, 0);
        if (v) result = strdup(v);
    }
    sqlite3_finalize(stmt);
    return result;
}

/* Write channel_state */
int channel_set_config(ChannelCtx *ctx, const char *key, const char *value) {
    if (!ctx || !key || !value) return -1;
    const char *sql =
        "INSERT OR REPLACE INTO channel_state(channel_name, key, value)"
        " VALUES(?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, ctx->channel_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Get oldest pending outbox row.
 *
 * Addressed by row id + session_id + channel_name — never by agent name, and
 * the channel runner holds no cached agent_name either. `cclaw rename-agent`
 * relies on this: the rename cascades through sessions.agent_name while
 * outbox rows and routes keep pointing at the same ids, so a delivery
 * in flight across a rename still lands. Don't introduce an agent-name
 * cache on this path. */
ChannelOutboxRow *channel_next_outbox(ChannelCtx *ctx) {
    if (!ctx) return NULL;
    const char *sql =
        "SELECT id, session_id, payload, deliver_mode FROM channel_outbox"
        " WHERE channel_name=? AND status='pending' AND next_attempt_at <= unixepoch()"
        " ORDER BY id ASC LIMIT 1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, ctx->channel_name, -1, SQLITE_STATIC);
    ChannelOutboxRow *row = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        row = calloc(1, sizeof(*row));
        if (row) {
            row->id = sqlite3_column_int64(stmt, 0);
            row->session_id = sqlite3_column_int64(stmt, 1);
            const char *p = (const char *)sqlite3_column_text(stmt, 2);
            row->payload = p ? strdup(p) : NULL;
            row->deliver_mode = sqlite3_column_int(stmt, 3);
        }
    }
    sqlite3_finalize(stmt);
    return row;
}

void channel_outbox_row_free(ChannelOutboxRow *row) {
    if (!row) return;
    free(row->payload);
    free(row);
}

/* Mark a row handed to JS so next_outbox doesn't re-deliver it while the
 * async send is in flight. Only moves pending rows — a row JS already
 * failed stays failed. */
int channel_dispatch_outbox(ChannelCtx *ctx, int64_t id) {
    if (!ctx) return -1;
    const char *sql =
        "UPDATE channel_outbox SET status='sending'"
        " WHERE id=? AND channel_name=? AND status='pending';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, ctx->channel_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Crash recovery at runner startup: rows stuck 'sending' (process died
 * mid-send) go back to 'pending' for at-least-once redelivery. */
int channel_reset_outbox(ChannelCtx *ctx) {
    if (!ctx) return -1;
    const char *sql =
        "UPDATE channel_outbox SET status='pending'"
        " WHERE channel_name=? AND status='sending';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, ctx->channel_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int channel_media_dir(const char *db_path, const char *channel,
                      char *out, size_t cap) {
    if (!channel || !channel[0] || strchr(channel, '/')) return -1;
    const char *dbp = db_path ? db_path : ".";
    const char *slash = strrchr(dbp, '/');
    int n = slash ? snprintf(out, cap, "%.*s/media", (int)(slash - dbp), dbp)
                  : snprintf(out, cap, "media");
    if (n < 0 || (size_t)n >= cap) return -1;
    mkdir(out, 0700);
    int m = snprintf(out + n, cap - (size_t)n, "/%s", channel);
    if (m < 0 || (size_t)m >= cap - (size_t)n) return -1;
    mkdir(out, 0700);
    return 0;
}

/* A file row (channel_send's `file`) owns a copy in the media spool. Once
 * the row is delivered or has failed for good, nothing will read the copy
 * again. Only a path inside this channel's own spool is ever removed. */
static void outbox_file_remove(ChannelCtx *ctx, int64_t id) {
    char dir[1024];
    sqlite3_stmt *s;
    if (channel_media_dir(ctx->db_path, ctx->channel_name, dir, sizeof(dir)) != 0 ||
        sqlite3_prepare_v2(ctx->db,
            "SELECT json_extract(payload,'$.file.path') FROM channel_outbox"
            " WHERE id=? AND channel_name=?;", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_text(s, 2, ctx->channel_name, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(s, 0);
        size_t dl = strlen(dir);
        if (path && strncmp(path, dir, dl) == 0 && path[dl] == '/' &&
            !strchr(path + dl + 1, '/'))
            unlink(path);
    }
    sqlite3_finalize(s);
}

/* Mark delivered */
int channel_ack_outbox(ChannelCtx *ctx, int64_t id) {
    if (!ctx) return -1;
    outbox_file_remove(ctx, id);
    const char *sql =
        "UPDATE channel_outbox SET status='delivered', acked_at=unixepoch()"
        " WHERE id=? AND channel_name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, ctx->channel_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Tell the sending session its message never made it. Called only from the
 * status flip below, which is the single choke point for permanent failure
 * (the C drain loop and the JS channel.failOutbox binding both land here). */
static void notify_send_failure(ChannelCtx *ctx, int64_t id, const char *status) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT o.session_id, json_extract(o.payload,'$.chat_id')"
            " FROM channel_outbox o JOIN sessions s ON s.id=o.session_id"
            " WHERE o.id=? AND o.channel_name=? AND o.session_id>0;",
            -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, id);
    sqlite3_bind_text(s, 2, ctx->channel_name, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        int64_t sid = sqlite3_column_int64(s, 0);
        const char *chat = (const char *)sqlite3_column_text(s, 1);
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "channel_send to (%s, %s) failed permanently: outbox id %lld, %s",
                 ctx->channel_name, chat ? chat : "unknown",
                 (long long)id, status);
        if (inbox_insert(ctx->db, sid, "system", NULL, msg) > 0)
            wake_external(ctx->db_path);
    }
    sqlite3_finalize(s);
}

/* Mark failed */
int channel_fail_outbox(ChannelCtx *ctx, int64_t id, const char *error) {
    if (!ctx) return -1;
    /* The status flip is the once-only guard: a row already 'failed: ...'
     * is not re-flipped, so the notice is written exactly once even if both
     * the runner loop and a JS handler try to fail the same row. */
    const char *sql =
        "UPDATE channel_outbox SET status=?"
        " WHERE id=? AND channel_name=? AND status NOT LIKE 'failed:%';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    /* Store error in status field as "failed:<reason>" */
    char status_buf[256];
    snprintf(status_buf, sizeof(status_buf), "failed: %s", error ? error : "unknown");
    sqlite3_bind_text(stmt, 1, status_buf, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_bind_text(stmt, 3, ctx->channel_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    int flipped = sqlite3_changes(ctx->db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    if (flipped > 0) {
        outbox_file_remove(ctx, id);
        notify_send_failure(ctx, id, status_buf);
    }
    return 0;
}

/* Reschedule a failed send for retry: bump attempt count, return to
 * pending with a future next_attempt_at. Non-blocking backoff. */
int channel_retry_outbox(ChannelCtx *ctx, int64_t id, int delay_sec) {
    if (!ctx) return -1;
    const char *sql =
        "UPDATE channel_outbox"
        " SET status='pending', attempts=attempts+1, next_attempt_at=unixepoch()+?"
        " WHERE id=? AND channel_name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(stmt, 1, delay_sec);
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_bind_text(stmt, 3, ctx->channel_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Re-deliver immediately at a degraded mode after a format rejection.
 * The ladder only descends (formatted → plain), so this can't loop. */
int channel_retry_outbox_mode(ChannelCtx *ctx, int64_t id, int mode) {
    if (!ctx) return -1;
    const char *sql =
        "UPDATE channel_outbox"
        " SET status='pending', deliver_mode=?, next_attempt_at=0"
        " WHERE id=? AND channel_name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int(stmt, 1, mode);
    sqlite3_bind_int64(stmt, 2, id);
    sqlite3_bind_text(stmt, 3, ctx->channel_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

/* Current retry attempt count for an outbox row, or 0 if unknown. */
int channel_outbox_attempts(ChannelCtx *ctx, int64_t id) {
    if (!ctx) return 0;
    const char *sql =
        "SELECT attempts FROM channel_outbox WHERE id=? AND channel_name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, ctx->channel_name, -1, SQLITE_STATIC);
    int val = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        val = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return val;
}

/* Current deliver_mode of an outbox row, or 0 if unknown. */
int channel_outbox_mode(ChannelCtx *ctx, int64_t id) {
    if (!ctx) return 0;
    const char *sql =
        "SELECT deliver_mode FROM channel_outbox WHERE id=? AND channel_name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, ctx->channel_name, -1, SQLITE_STATIC);
    int val = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        val = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return val;
}


/* ── Per-channel IPC paths ─────────────────────────────────────── */

/* Format: <db_path_without_.db>.<channel_name><suffix> */
static char *channel_ipc_path(const char *db_path, const char *channel_name,
                              const char *suffix) {
    if (!db_path || !channel_name) return NULL;
    if (strcmp(db_path, ":memory:") == 0) return NULL;
    size_t db_len = strlen(db_path);
    size_t ch_len = strlen(channel_name);
    size_t sfx_len = strlen(suffix);
    size_t base_len = db_len;
    if (db_len > 3 && strcmp(db_path + db_len - 3, ".db") == 0)
        base_len = db_len - 3;
    char *path = malloc(base_len + 1 + ch_len + sfx_len + 1);
    if (!path) return NULL;
    memcpy(path, db_path, base_len);
    path[base_len] = '.';
    memcpy(path + base_len + 1, channel_name, ch_len);
    memcpy(path + base_len + 1 + ch_len, suffix, sfx_len + 1);
    return path;
}

char *channel_outbox_fifo_path(const char *db_path, const char *channel_name) {
    return channel_ipc_path(db_path, channel_name, ".pipe");
}

char *channel_uds_path(const char *db_path, const char *channel_name) {
    return channel_ipc_path(db_path, channel_name, ".sock");
}

int channel_outbox_fifo_open(const char *db_path, const char *channel_name) {
    char *path = channel_outbox_fifo_path(db_path, channel_name);
    if (!path) return -1;
    unlink(path);
    if (mkfifo(path, 0600) != 0 && errno != EEXIST) { free(path); return -1; }
    /* O_RDWR, not O_RDONLY: same as wake_fifo_open — once the daemon's
     * first open-write-close cycle ends, a read-only fd sits at perpetual
     * EOF and curl_multi_wait returns immediately forever (busy-loop).
     * Holding our own write end keeps the FIFO out of zero-writer state. */
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    free(path);
    return fd;
}

void channel_outbox_fifo_close(int fd, const char *db_path, const char *channel_name) {
    if (fd >= 0) close(fd);
    char *path = channel_outbox_fifo_path(db_path, channel_name);
    if (path) { unlink(path); free(path); }
}

int channel_outbox_wake(const char *db_path, const char *channel_name) {
    char *path = channel_outbox_fifo_path(db_path, channel_name);
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_NONBLOCK);
    free(path);
    if (fd < 0) return -1;
    char c = 1;
    (void)write(fd, &c, 1);
    close(fd);
    return 0;
}

/* ── Outbox insert (daemon side) ───────────────────────────────── */

int channel_notify_operator(sqlite3 *db, const char *db_path, int64_t session_id,
                            const char *text) {
    if (!db || !text) return -1;

    /* Where the session lives. Channel-less sessions (CLI, sub-agents) have
     * no operator to reach this way: no-op. */
    char ch[64] = "", chat[64] = "", agent[64] = "";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT channel_name, chat_id, COALESCE(agent_name,'?') FROM sessions"
            " WHERE id=?1 AND channel_name IS NOT NULL AND chat_id IS NOT NULL;",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(s, 1, session_id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        snprintf(ch, sizeof(ch), "%s", sqlite3_column_text(s, 0));
        snprintf(chat, sizeof(chat), "%s", sqlite3_column_text(s, 1));
        snprintf(agent, sizeof(agent), "%s", sqlite3_column_text(s, 2));
    }
    sqlite3_finalize(s);
    if (!ch[0]) return 0;

    /* A system notice is for the operator, not for whoever shares the chat:
     * a session can be bound to a group where the people reading are not the
     * ones running the box. It goes where the channel's operator reads
     * (channel_operator_queue: admin_chats, else a DM to each admin — the
     * destination approval prompts use) and names the agent and session it
     * is about, since it no longer arrives in their chat. Only a channel
     * with neither configured falls back to the session's own chat, so a
     * plain 1:1 setup needs no extra config. */
    int rc = 0, queued = 0;
    size_t cap = strlen(text) + sizeof(agent) + 48;
    char *full = malloc(cap);
    if (!full) return -1;
    snprintf(full, cap, "%s (agent %s, session #%lld)", text, agent,
             (long long)session_id);
    queued = channel_operator_queue(db, ch, session_id, full);
    free(full);
    if (queued == 0) {
        if (sqlite3_prepare_v2(db,
                "INSERT INTO channel_outbox(channel_name, session_id, payload)"
                " VALUES(?1, ?2, json_object('chat_id', ?3, 'text', ?4));",
                -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, ch, -1, SQLITE_STATIC);
            sqlite3_bind_int64(s, 2, session_id);
            sqlite3_bind_text(s, 3, chat, -1, SQLITE_STATIC);
            sqlite3_bind_text(s, 4, text, -1, SQLITE_STATIC);
            if (sqlite3_step(s) == SQLITE_DONE) queued++; else rc = -1;
            sqlite3_finalize(s);
        } else rc = -1;
    }

    if (queued && db_path) channel_outbox_wake(db_path, ch);
    return rc;
}

