#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "web.h"
#include "buf.h"
#include "cclaw.h"
#include "channel_api.h"
#include "civetweb.h"
#include "dashboard.h"
#include "db.h"
#include "json_escape.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
static pthread_mutex_t s_db_mu = PTHREAD_MUTEX_INITIALIZER;

void web_db_lock(void) { pthread_mutex_lock(&s_db_mu); }
void web_db_unlock(void) { pthread_mutex_unlock(&s_db_mu); }
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

/* Append s JSON-string-escaped (no surrounding quotes). */
static void buf_append_json(Buf *b, const char *s) {
    size_t n = strlen(s), cap = n * 6 + 1;
    char *tmp = malloc(cap);
    if (!tmp) { b->oom = 1; return; }
    size_t w = json_escape(tmp, cap, s, n);
    buf_append(b, tmp, w);
    free(tmp);
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

    /* Build envelope: {"method","path","headers":{},"body"}. Every piece
     * comes from an unauthenticated client, and JSON escaping can grow a byte
     * sixfold, so this is a growable buffer — the fixed one it replaces
     * (2x body + 8K) could be overrun by a single long header. */
    Buf eb = {0};
    buf_append_str(&eb, "{\"method\":\"");
    if (ri->request_method) buf_append_json(&eb, ri->request_method);
    buf_append_str(&eb, "\",\"path\":\"");
    buf_append_json(&eb, uri);
    if (ri->query_string && ri->query_string[0]) {
        buf_append_char(&eb, '?');
        buf_append_json(&eb, ri->query_string);
    }
    buf_append_str(&eb, "\",\"headers\":{");
    for (int i = 0; i < ri->num_headers; i++) {
        if (i > 0) buf_append_char(&eb, ',');
        buf_append_char(&eb, '"');
        buf_append_json(&eb, ri->http_headers[i].name);
        buf_append_str(&eb, "\":\"");
        buf_append_json(&eb, ri->http_headers[i].value);
        buf_append_char(&eb, '"');
    }
    buf_append_str(&eb, "},\"body\":\"");
    buf_append_json(&eb, body_buf);
    buf_append_str(&eb, "\"}");
    size_t pos = eb.len;
    char *env = buf_take(&eb);
    if (!env) return hook_reply(conn, 500, "{\"error\":\"oom\"}");

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

    web_db_lock();

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
    web_db_unlock();

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

/* ── /share/<token>[/<name>] — a file an agent put behind a link ──
 * (share_file, tool_share.c). The token is the capability; there is no other
 * authentication, by design — the link is meant to be opened by whoever it
 * was given to. Everything that is not a live share is the same 404.
 *
 * The file is the agent's, so its content is untrusted, and this server also
 * hosts /admin on the same origin. Nothing here may run as a page: only
 * images and PDFs (recognized by their bytes) and plain text are shown
 * inline, each under a sandboxing CSP and nosniff; anything else is a
 * download. HTML is therefore text, never markup. */

static int share_404(struct mg_connection *conn) {
    mg_printf(conn, "HTTP/1.1 404 Not Found\r\nContent-Type: text/plain\r\n"
                    "Content-Length: 10\r\nConnection: close\r\n\r\nnot found\n");
    return 404;
}

static int handle_share(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    const struct mg_request_info *ri = mg_get_request_info(conn);
    const char *uri = ri->local_uri ? ri->local_uri : "";
    const char *method = ri->request_method ? ri->request_method : "";
    int head = strcmp(method, "HEAD") == 0;
    if ((!head && strcmp(method, "GET") != 0) || strncmp(uri, "/share/", 7) != 0)
        return share_404(conn);

    char token[33];
    size_t tl = strspn(uri + 7, "0123456789abcdef");
    if (tl != 32 || (uri[7 + tl] != '\0' && uri[7 + tl] != '/'))
        return share_404(conn);
    memcpy(token, uri + 7, 32);
    token[32] = '\0';

    char path[PATH_MAX] = "", name[96] = "";
    web_db_lock();
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(s_db,
            "SELECT path, name FROM shares"
            " WHERE token=?1 AND expires_at > unixepoch();",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, token, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            snprintf(path, sizeof(path), "%s", (const char *)sqlite3_column_text(st, 0));
            snprintf(name, sizeof(name), "%s", (const char *)sqlite3_column_text(st, 1));
        }
        sqlite3_finalize(st);
    }
    web_db_unlock();
    if (!path[0]) return share_404(conn);

    /* The agent can still write its workspace. What was shared is the file
     * at this resolved path; if the path now leads anywhere else (replaced
     * by a symlink, a directory swapped on the way) it is not that file.
     * Checked on the open descriptor, like the share itself. */
    char dir[PATH_MAX], real[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    struct stat sb;
    if (!slash || slash == dir) return share_404(conn);
    *slash = '\0';
    int fd = util_open_inside(dir, path, real, sizeof(real));
    if (fd < 0) return share_404(conn);
    unsigned char first[4096];
    ssize_t fn = 0;
    if (strcmp(real, path) != 0 || fstat(fd, &sb) != 0 || !S_ISREG(sb.st_mode) ||
        (fn = read(fd, first, sizeof(first))) < 0) {
        close(fd);
        return share_404(conn);
    }

    const char *type = util_sniff_mime(first, (size_t)fn);
    const char *disp = "inline";
    if (!type) {
        if (memchr(first, 0, (size_t)fn)) {
            type = "application/octet-stream";
            disp = "attachment";
        } else {
            type = "text/plain; charset=utf-8";
        }
    }
    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %lld\r\n"
        "Content-Disposition: %s; filename=\"%s\"\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Content-Security-Policy: sandbox; default-src 'none'\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n\r\n",
        type, (long long)sb.st_size, disp, name);
    /* Exactly Content-Length bytes: a file that grew is cut at the size
     * promised, one that shrank ends the connection short. */
    long long left = head ? 0 : (long long)sb.st_size;
    unsigned char buf[65536];
    ssize_t n = fn;
    const unsigned char *p = first;
    while (left > 0 && n > 0) {
        size_t w = (long long)n > left ? (size_t)left : (size_t)n;
        if (mg_write(conn, p, w) <= 0) break;
        left -= (long long)w;
        n = left > 0 ? read(fd, buf, sizeof(buf)) : 0;
        p = buf;
    }
    close(fd);
    return 200;
}

int web_start(Config *cfg, const char *db_path) {
    if (!db_path) return -1;
    s_db = db_open(db_path);
    if (!s_db) return -1;
    s_db_path = strdup(db_path);
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
    if (!s_ctx) { web_stop(); return -1; }

    mg_set_request_handler(s_ctx, "/hook/", handle_hook, NULL);
    mg_set_request_handler(s_ctx, "/share/", handle_share, NULL);
    if (dashboard_register(s_ctx, s_db) != 0) {
        web_stop();
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
    /* mg_stop joined the worker threads, so nothing holds the handle now. */
    if (s_db) { sqlite3_close(s_db); s_db = NULL; }
    free(s_db_path);
    s_db_path = NULL;
}
