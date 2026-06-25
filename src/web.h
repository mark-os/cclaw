#ifndef CCLAW_WEB_H
#define CCLAW_WEB_H

#include "types.h"
#include "sqlite3.h"

/* Start civetweb server on cfg->web_port. db_path locates the channel
 * runner request sockets for /hook/ proxying. Returns 0 on success. */
int web_start(Config *cfg, sqlite3 *db, const char *db_path);

/* Stop civetweb server and free resources. */
void web_stop(void);

#endif
