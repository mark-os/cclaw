#include "db.h"
#include <stdio.h>

static const char *SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS sessions ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  name TEXT,"
    "  leaf_id INTEGER DEFAULT -1,"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  updated_at INTEGER NOT NULL DEFAULT (unixepoch())"
    ");"
    "CREATE TABLE IF NOT EXISTS entries ("
    "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "  parent_id INTEGER NOT NULL DEFAULT -1,"
    "  session_id INTEGER NOT NULL REFERENCES sessions(id),"
    "  created_at INTEGER NOT NULL DEFAULT (unixepoch()),"
    "  role INTEGER NOT NULL,"
    "  content TEXT,"
    "  tool_calls_json TEXT,"
    "  tool_result_json TEXT"
    ");"
    "CREATE INDEX IF NOT EXISTS idx_entries_session ON entries(session_id);"
    "CREATE INDEX IF NOT EXISTS idx_entries_parent ON entries(parent_id);";

sqlite3 *db_open(const char *path) {
    sqlite3 *db = NULL;
    int rc = sqlite3_open(path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_open: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return NULL;
    }

    /* V4: WAL mode */
    char *err = NULL;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    /* V4: busy_timeout >= 5000ms */
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    /* Foreign keys */
    sqlite3_exec(db, "PRAGMA foreign_keys=ON;", NULL, NULL, &err);
    if (err) { sqlite3_free(err); err = NULL; }

    /* Create tables */
    rc = sqlite3_exec(db, SCHEMA_SQL, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "db_open schema: %s\n", err);
        sqlite3_free(err);
        sqlite3_close(db);
        return NULL;
    }

    return db;
}

void db_close(sqlite3 *db) {
    if (db) sqlite3_close(db);
}
