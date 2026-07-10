#ifndef CCLAW_DASHBOARD_H
#define CCLAW_DASHBOARD_H

#include <sqlite3.h>

struct mg_context;

/* Register the /admin dashboard on a running civetweb context and ensure the
 * web_admin_token config value exists (generated from /dev/urandom on first
 * start; `cclaw dashboard` prints the tokenized URL). Returns 0 on success. */
int dashboard_register(struct mg_context *ctx, sqlite3 *db);

/* Malloc'd "http://<first non-loopback IPv4>:<web_port>/admin?token=…", or
 * NULL before the daemon has minted a token. Shared by `cclaw dashboard`
 * and the Telegram /admin command. */
char *dashboard_url(sqlite3 *db);

#endif
