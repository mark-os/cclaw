#include "web.h"
#include "cclaw.h"
#include "civetweb.h"
#include "cJSON.h"
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

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", CCLAW_VERSION);
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)uptime);

    /* Active sessions with state, lock holders, inbox depths */
    cJSON *sessions = cJSON_AddArrayToObject(root, "sessions");
    if (s_db) {
        sqlite3_stmt *stmt;
        const char *sql =
            "SELECT s.id, s.name, s.state, s.lock_holder, s.error_count, s.updated_at,"
            " (SELECT COUNT(*) FROM inbox i WHERE i.session_id=s.id AND i.consumed=0)"
            " FROM sessions s ORDER BY s.updated_at DESC;";
        if (sqlite3_prepare_v2(s_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                cJSON *s = cJSON_CreateObject();
                cJSON_AddNumberToObject(s, "id", (double)sqlite3_column_int64(stmt, 0));
                const char *name = (const char *)sqlite3_column_text(stmt, 1);
                cJSON_AddStringToObject(s, "name", name ? name : "");
                const char *state = (const char *)sqlite3_column_text(stmt, 2);
                cJSON_AddStringToObject(s, "state", state ? state : "idle");
                const char *holder = (const char *)sqlite3_column_text(stmt, 3);
                if (holder)
                    cJSON_AddStringToObject(s, "lock_holder", holder);
                else
                    cJSON_AddNullToObject(s, "lock_holder");
                cJSON_AddNumberToObject(s, "error_count", (double)sqlite3_column_int(stmt, 4));
                cJSON_AddNumberToObject(s, "updated_at", (double)sqlite3_column_int64(stmt, 5));
                cJSON_AddNumberToObject(s, "inbox_depth", (double)sqlite3_column_int(stmt, 6));
                cJSON_AddItemToArray(sessions, s);
            }
            sqlite3_finalize(stmt);
        }

        /* Aggregate state metrics */
        cJSON *metrics = cJSON_AddObjectToObject(root, "state_metrics");
        sqlite3_stmt *mstmt;
        const char *msql = "SELECT state, COUNT(*) FROM sessions GROUP BY state;";
        if (sqlite3_prepare_v2(s_db, msql, -1, &mstmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(mstmt) == SQLITE_ROW) {
                const char *st = (const char *)sqlite3_column_text(mstmt, 0);
                int cnt = sqlite3_column_int(mstmt, 1);
                if (st) cJSON_AddNumberToObject(metrics, st, (double)cnt);
            }
            sqlite3_finalize(mstmt);
        }
    }

    /* Sub-agent status */
    cJSON_AddArrayToObject(root, "sub_agents");

    char *body = cJSON_PrintUnformatted(root);
    int len = (int)strlen(body);

    mg_printf(conn,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s", len, body);

    free(body);
    cJSON_Delete(root);
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
