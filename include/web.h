#ifndef CCLAW_WEB_H
#define CCLAW_WEB_H

#include "types.h"
#include "sqlite3.h"

/* Start civetweb server on cfg->web_port. Returns 0 on success. */
int web_start(Config *cfg, sqlite3 *db);

/* Stop civetweb server and free resources. */
void web_stop(void);

#endif
