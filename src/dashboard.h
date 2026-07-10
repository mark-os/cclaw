#ifndef CCLAW_DASHBOARD_H
#define CCLAW_DASHBOARD_H

#include <sqlite3.h>

struct mg_context;

/* Register the /admin dashboard on a running civetweb context and ensure the
 * web_admin_token config value exists (generated from /dev/urandom on first
 * start; `cclaw dashboard` prints the tokenized URL). Returns 0 on success. */
int dashboard_register(struct mg_context *ctx, sqlite3 *db);

#endif
