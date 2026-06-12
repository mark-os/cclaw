#ifndef CCLAW_TEST_UTIL_H
#define CCLAW_TEST_UTIL_H

#include "db.h"

/* Convenience: open + ensure schema. */
static inline sqlite3 *test_db_open(const char *path) {
    sqlite3 *db = db_open(path);
    if (db) db_ensure_schema(db);
    return db;
}

#endif /* CCLAW_TEST_UTIL_H */
