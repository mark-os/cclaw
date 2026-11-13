#ifndef CCLAW_WEB_H
#define CCLAW_WEB_H

#include "types.h"
#include "sqlite3.h"

/* Start civetweb server on cfg->web_port. The server opens its OWN database
 * handle from db_path and never touches the event loop's: a transaction
 * belongs to a handle, so a shared one lets a request on a web thread end or
 * join a transaction the loop has open. db_path also locates the channel
 * runner request sockets for /hook/ proxying. Returns 0 on success. */
int web_start(Config *cfg, const char *db_path);

/* The web server's handle is shared by civetweb's worker threads, so a
 * handler holds this lock for the whole request it serves — one request's
 * statements never interleave with another's. Every handler that reads or
 * writes the database takes it. */
void web_db_lock(void);
void web_db_unlock(void);

/* Stop civetweb server and free resources. */
void web_stop(void);

#endif
