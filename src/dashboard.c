#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "dashboard.h"
#include "util.h"
#include "civetweb.h"
#include "config_registry.h"
#include "explore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The operator's pages: /admin (the dashboard) and /admin/data (the data
 * explorer), and their credential. Both pages and their API live in explore.c
 * (specs/explorer.md). Auth is a single generated token — a cookie for the
 * session, ?token= for the first visit (LAN threat model; TLS/tunnelling
 * later). The links are config_admin_url(). */

static sqlite3 *s_db;

static int token_eq(const char *a, const char *b) {
    if (!a || !b) return 0;
    size_t la = strlen(a), lb = strlen(b);
    unsigned char diff = (la == lb) ? 0 : 1;
    for (size_t i = 0; i < la; i++)
        diff |= (unsigned char)(a[i] ^ b[i % (lb ? lb : 1)]);
    return diff == 0;
}

static int ensure_token(sqlite3 *db) {
    char *cur = config_get(db, "web_admin_token");
    int have = (cur && cur[0]);
    free(cur);
    if (have) return 0;
    unsigned char rnd[16];
    if (util_random(rnd, sizeof(rnd)) != 0) return -1;
    char tok[33];
    for (int i = 0; i < 16; i++)
        snprintf(tok + i * 2, 3, "%02x", rnd[i]);
    return config_set(db, "web_admin_token", tok);
}

/* 0 = unauthenticated, 1 = valid cookie, 2 = valid ?token= (set cookie) */
int dashboard_auth_check(struct mg_connection *conn, int allow_query) {
    char *want = config_get(s_db, "web_admin_token");
    if (!want || !want[0]) { free(want); return 0; }
    int ok = 0;
    char got[80];
    const char *cookie = mg_get_header(conn, "Cookie");
    if (cookie && mg_get_cookie(cookie, "cclaw_admin", got, sizeof(got)) > 0 &&
        token_eq(got, want))
        ok = 1;
    if (!ok && allow_query) {
        const struct mg_request_info *ri = mg_get_request_info(conn);
        if (ri->query_string &&
            mg_get_var(ri->query_string, strlen(ri->query_string),
                       "token", got, sizeof(got)) > 0 &&
            token_eq(got, want))
            ok = 2;
    }
    free(want);
    return ok;
}

static int handle_admin(struct mg_connection *conn, void *cbdata) {
    (void)cbdata;
    return explore_handle(conn, s_db);
}

int dashboard_register(struct mg_context *ctx, sqlite3 *db) {
    if (!ctx || !db) return -1;
    s_db = db;
    if (ensure_token(db) != 0) return -1;
    mg_set_request_handler(ctx, "/admin", handle_admin, NULL);
    return 0;
}
