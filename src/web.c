#include "web.h"
#include "cclaw.h"
#include "civetweb.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct mg_context *s_ctx;
static time_t s_start_time;
static sqlite3 *s_db;

static int handle_status(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    time_t now = time(NULL);
    long uptime = (long)(now - s_start_time);

    /* Count active sessions */
    int sessions = 0;
    if (s_db) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(s_db, "SELECT COUNT(*) FROM sessions", -1, &stmt, NULL) == SQLITE_OK) {
            if (sqlite3_step(stmt) == SQLITE_ROW)
                sessions = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
        }
    }

    char body[512];
    int len = snprintf(body, sizeof(body),
        "{\"version\":\"%s\",\"uptime_seconds\":%ld,\"sessions\":%d}",
        CCLAW_VERSION, uptime, sessions);

    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s", len, body);
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
