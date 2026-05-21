#ifndef CCLAW_DB_H
#define CCLAW_DB_H

#include "sqlite3.h"

/* Open DB at path, set WAL mode + pragmas, create tables.
 * Returns NULL on failure. */
sqlite3 *db_open(const char *path);

/* Close DB handle. */
void db_close(sqlite3 *db);

#endif
