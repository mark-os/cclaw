#define _GNU_SOURCE
#include "channel_api.h"
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

/* Get oldest pending outbox row */
ChannelOutboxRow *channel_next_outbox(ChannelCtx *ctx) {
    if (!ctx) return NULL;
    const char *sql =
        "SELECT id, session_id, payload FROM channel_outbox"
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

/* Mark delivered */
int channel_ack_outbox(ChannelCtx *ctx, int64_t id) {
    if (!ctx) return -1;
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

/* Mark failed */
int channel_fail_outbox(ChannelCtx *ctx, int64_t id, const char *error) {
    if (!ctx) return -1;
    const char *sql =
        "UPDATE channel_outbox SET status=?"
        " WHERE id=? AND channel_name=?;";
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
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
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

