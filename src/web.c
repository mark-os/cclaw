#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "web.h"
#include "buf.h"
#include "cclaw.h"
#include "channel_api.h"
#include "civetweb.h"
#include "dashboard.h"
#include "json_escape.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define HOOK_BODY_MAX 65536
#define HOOK_REPLY_MAX (256 * 1024)
#define HOOK_TIMEOUT_SEC 10

static struct mg_context *s_ctx;
static time_t s_start_time;
static sqlite3 *s_db;
static char *s_db_path;

/* ── /hook/<channel> — dumb proxy to the channel runner's UDS ────
 * The daemon owns the listen port; channel knowledge (verification,
 * challenges, parsing) lives in the runner's JS. We forward the parsed
 * request envelope and relay the "status\nbody" reply. */

/* Connect to the runner's request socket. Returns fd or -1. */
static int hook_connect(const char *channel) {
    char *path = channel_uds_path(s_db_path, channel);
    if (!path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { free(path); return -1; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    free(path);
    struct timeval tv = {HOOK_TIMEOUT_SEC, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int hook_reply(struct mg_connection *conn, int status, const char *body) {
    mg_printf(conn,
        "HTTP/1.1 %d\r\nContent-Type: application/json\r\n"
        "Content-Length: %zu\r\n\r\n%s",
        status, strlen(body), body);
    return status;
}

static int handle_hook(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);

    /* /hook/<channel>[/...] — extract and validate the channel name */
    const char *uri = ri->local_uri ? ri->local_uri : "";
    if (strncmp(uri, "/hook/", 6) != 0)
        return hook_reply(conn, 404, "{\"error\":\"not found\"}");
    char channel[64];
    size_t cl = 0;
    for (const char *p = uri + 6; *p && *p != '/' && cl < sizeof(channel) - 1; p++) {
        char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return hook_reply(conn, 404, "{\"error\":\"bad channel\"}");
        channel[cl++] = c;
    }
    channel[cl] = '\0';
    if (cl == 0)
        return hook_reply(conn, 404, "{\"error\":\"bad channel\"}");

    /* Read body */
    char body_buf[HOOK_BODY_MAX];
    int body_len = 0;
    if (ri->content_length > 0 && ri->content_length < (long long)sizeof(body_buf))
        body_len = mg_read(conn, body_buf, (size_t)ri->content_length);
    else if (ri->content_length < 0)
        body_len = mg_read(conn, body_buf, sizeof(body_buf) - 1);
    if (body_len < 0) body_len = 0;
    body_buf[body_len] = '\0';

    /* Build envelope: {"method","path","headers":{},"body"} */
    size_t cap = (size_t)body_len * 2 + 8192;
    char *env = malloc(cap);
    if (!env) return hook_reply(conn, 500, "{\"error\":\"oom\"}");
    size_t pos = 0;
    pos += (size_t)snprintf(env + pos, cap - pos, "{\"method\":\"");
    if (ri->request_method)
        pos += json_escape(env + pos, cap - pos, ri->request_method, strlen(ri->request_method));
    pos += (size_t)snprintf(env + pos, cap - pos, "\",\"path\":\"");
    pos += json_escape(env + pos, cap - pos, uri, strlen(uri));
    if (ri->query_string && ri->query_string[0]) {
        pos += (size_t)snprintf(env + pos, cap - pos, "?");
        pos += json_escape(env + pos, cap - pos, ri->query_string, strlen(ri->query_string));
    }
    pos += (size_t)snprintf(env + pos, cap - pos, "\",\"headers\":{");
    for (int i = 0; i < ri->num_headers && pos + 512 < cap; i++) {
        if (i > 0) env[pos++] = ',';
        env[pos++] = '"';
        pos += json_escape(env + pos, cap - pos, ri->http_headers[i].name,
                           strlen(ri->http_headers[i].name));
        pos += (size_t)snprintf(env + pos, cap - pos, "\":\"");
        pos += json_escape(env + pos, cap - pos, ri->http_headers[i].value,
                           strlen(ri->http_headers[i].value));
        env[pos++] = '"';
    }
    pos += (size_t)snprintf(env + pos, cap - pos, "},\"body\":\"");
    pos += json_escape(env + pos, cap - pos, body_buf, (size_t)body_len);
    pos += (size_t)snprintf(env + pos, cap - pos, "\"}");

    /* Forward to the runner; this civetweb worker thread blocks, which is
     * the natural model here */
    int fd = hook_connect(channel);
    if (fd < 0) {
        free(env);
        return hook_reply(conn, 502, "{\"error\":\"channel unavailable\"}");
    }
    size_t off = 0;
    while (off < pos) {
        ssize_t n = write(fd, env + off, pos - off);
        if (n <= 0) { if (n < 0 && errno == EINTR) continue; break; }
        off += (size_t)n;
    }
    free(env);
    if (off < pos) {
        close(fd);
        return hook_reply(conn, 502, "{\"error\":\"channel write failed\"}");
    }
    shutdown(fd, SHUT_WR);

    /* Read "status\nbody" reply */
    char *reply = malloc(HOOK_REPLY_MAX);
    if (!reply) { close(fd); return hook_reply(conn, 500, "{\"error\":\"oom\"}"); }
    size_t rlen = 0;
    for (;;) {
        if (rlen + 1 >= HOOK_REPLY_MAX) break;
        ssize_t n = read(fd, reply + rlen, HOOK_REPLY_MAX - rlen - 1);
        if (n > 0) { rlen += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
    reply[rlen] = '\0';

    if (rlen == 0) {
        free(reply);
        return hook_reply(conn, 504, "{\"error\":\"channel timeout\"}");
    }
    int status = atoi(reply);
    if (status < 100 || status > 599) status = 502;
    char *nl = strchr(reply, '\n');
    int rc = hook_reply(conn, status, nl ? nl + 1 : "");
    free(reply);
    return rc;
}

static int handle_status(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    time_t now = time(NULL);
    long uptime = (long)(now - s_start_time);

    Buf b = {0};

    buf_appendf(&b, "version: %s\nuptime: %lds\n", CCLAW_VERSION, uptime);

    /* Unauthenticated `/` is liveness only — the session table (names,
     * states, inbox depths) is operator data, gated behind the same
     * web_admin_token as /admin (cookie or ?token=). */
    if (s_db && dashboard_auth_check(conn, 1) != 0) {
        buf_appendf(&b, "\nsessions:\nid|name|state|updated_at|inbox_depth\n");

        sqlite3_stmt *stmt;
        const char *sql =
            "SELECT s.id, s.name, s.state, s.updated_at,"
            " (SELECT COUNT(*) FROM inbox i WHERE i.session_id=s.id AND i.consumed=0)"
            " FROM sessions s ORDER BY s.updated_at DESC;";
        if (sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int64_t id = sqlite3_column_int64(stmt, 0);
                const char *name = (const char *)sqlite3_column_text(stmt, 1);
                const char *state = (const char *)sqlite3_column_text(stmt, 2);
                int64_t updated = sqlite3_column_int64(stmt, 3);
                int inbox = sqlite3_column_int(stmt, 4);
                buf_appendf(&b, "%lld|%s|%s|%lld|%d\n",
                    (long long)id, name ? name : "", state ? state : "idle",
                    (long long)updated, inbox);
            }
            sqlite3_finalize(stmt);
        }

        /* State metrics */
        sqlite3_stmt *mstmt;
        const char *msql = "SELECT state, COUNT(*) FROM sessions GROUP BY state;";
        if (sqlite3_prepare_v2(s_db, msql, -1, &mstmt, NULL) == SQLITE_OK) {
            buf_appendf(&b, "\nstate_metrics:\n");
            while (sqlite3_step(mstmt) == SQLITE_ROW) {
                const char *st = (const char *)sqlite3_column_text(mstmt, 0);
                int cnt = sqlite3_column_int(mstmt, 1);
                if (st) buf_appendf(&b, "%s: %d\n", st, cnt);
            }
            sqlite3_finalize(mstmt);
        }
    }

    char *out = buf_take(&b);
    if (!out) { mg_printf(conn, "HTTP/1.1 500\r\n\r\n"); return 500; }

    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s", (int)strlen(out), out);

    free(out);
    return 200;
}

int web_start(Config *cfg, sqlite3 *db, const char *db_path) {
    s_db = db;
    s_db_path = db_path ? strdup(db_path) : NULL;
    s_start_time = time(NULL);

    mg_init_library(0);

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", cfg->web_port);

    const char *options[] = {
        "listening_ports", port_str,
        "num_threads", "2",
        NULL
    };

    s_ctx = mg_start(NULL, NULL, options);
    if (!s_ctx) return -1;

    mg_set_request_handler(s_ctx, "/hook/", handle_hook, NULL);
    if (dashboard_register(s_ctx, db) != 0) {
        mg_stop(s_ctx);
        s_ctx = NULL;
        return -1;
    }
    mg_set_request_handler(s_ctx, "/", handle_status, NULL);
    return 0;
}

void web_stop(void) {
    if (s_ctx) {
        mg_stop(s_ctx);
        s_ctx = NULL;
    }
    mg_exit_library();
    free(s_db_path);
    s_db_path = NULL;
}
