#include "web.h"
#include "cclaw.h"
#include "civetweb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct mg_context *s_ctx;
static time_t s_start_time;
static sqlite3 *s_db;

static int handle_status(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    time_t now = time(NULL);
    long uptime = (long)(now - s_start_time);

    size_t cap = 4096, pos = 0;
    char *buf = malloc(cap);
    if (!buf) { mg_printf(conn, "HTTP/1.1 500\r\n\r\n"); return 500; }

    pos += (size_t)snprintf(buf + pos, cap - pos,
        "version: %s\nuptime: %lds\n", CCLAW_VERSION, uptime);

    if (s_db) {
        pos += (size_t)snprintf(buf + pos, cap - pos,
            "\nsessions:\nid|name|state|updated_at|inbox_depth\n");

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
                while (pos + 256 > cap) { cap *= 2; buf = realloc(buf, cap); }
                pos += (size_t)snprintf(buf + pos, cap - pos,
                    "%lld|%s|%s|%lld|%d\n",
                    (long long)id, name ? name : "", state ? state : "idle",
                    (long long)updated, inbox);
            }
            sqlite3_finalize(stmt);
        }

        /* State metrics */
        sqlite3_stmt *mstmt;
        const char *msql = "SELECT state, COUNT(*) FROM sessions GROUP BY state;";
        if (sqlite3_prepare_v2(s_db, msql, -1, &mstmt, NULL) == SQLITE_OK) {
            pos += (size_t)snprintf(buf + pos, cap - pos, "\nstate_metrics:\n");
            while (sqlite3_step(mstmt) == SQLITE_ROW) {
                const char *st = (const char *)sqlite3_column_text(mstmt, 0);
                int cnt = sqlite3_column_int(mstmt, 1);
                while (pos + 64 > cap) { cap *= 2; buf = realloc(buf, cap); }
                if (st) pos += (size_t)snprintf(buf + pos, cap - pos, "%s: %d\n", st, cnt);
            }
            sqlite3_finalize(mstmt);
        }
    }

    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s", (int)pos, buf);

    free(buf);
    return 200;
}

int web_start(Config *cfg, sqlite3 *db) {
    s_db = db;
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

    mg_set_request_handler(s_ctx, "/", handle_status, NULL);
    return 0;
}

void web_stop(void) {
    if (s_ctx) {
        mg_stop(s_ctx);
        s_ctx = NULL;
    }
    mg_exit_library();
}
