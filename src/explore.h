#ifndef CCLAW_EXPLORE_H
#define CCLAW_EXPLORE_H

/* The data explorer (specs/explorer.md), read side: a JSON API that
 * describes and pages through every table, and the one static page that
 * draws it. Reading is generic — nothing here knows a table by name except
 * the short hand lists that say what the schema does not (home order, links
 * the schema leaves undeclared, labels for numeric codes, hidden values).
 *
 * Every response body is produced by SQL (JSON1). Table and column names in
 * a request are only ever compared against the schema; the names that reach
 * SQL text are the schema's own, quoted. Values are bound parameters. */

#include "sqlite3.h"
#include <stdint.h>

struct mg_connection;

typedef struct {
    const char *table;   /* rows, row */
    const char *key;     /* row: JSON array of the key values, in key order */
    const char *filter;  /* rows: JSON object {column: value | null | [values]} */
    const char *after;   /* rows: the previous page's "next" (JSON array) */
    const char *search;  /* rows: substring of a text key column (case-insensitive) */
    int full;            /* rows: long text whole, as on a row page (a page that edits needs it) */
    int limit;           /* rows: 1..EXPLORE_MAX_LIMIT; 0 = default */
} ExploreQuery;

#define EXPLORE_DEFAULT_LIMIT 50
#define EXPLORE_MAX_LIMIT 200

/* Each returns an HTTP status and sets *out to a malloc'd JSON body (an
 * {"error": ...} object when the status is not 200). Read-only: plain
 * SELECTs in autocommit. */
int explore_meta(sqlite3 *db, char **out);
int explore_rows(sqlite3 *db, const ExploreQuery *q, char **out);
int explore_row(sqlite3 *db, const ExploreQuery *q, char **out);
/* Prompt-cache statistics from entries: per model, cache share and request
 * counts by gap since the session's previous request. days > 0 limits it to
 * requests of the last that many days; 0 = all history. */
int explore_cache(sqlite3 *db, int days, char **out);
/* One session's context and edits: its size (head + entries, in tokens, as
 * the next request would carry it) against the governing routing entry's
 * window, trim threshold and prune target — with what the entry itself sets,
 * NULL where the default applies — and every edit record on any of its
 * entries ($.turn_edits), newest first. 404 when there is no such session. */
int explore_session(sqlite3 *db, int64_t session_id, char **out);

/* 1 when table.column's value is never shown in any row (0 = shown, 2 =
 * hidden in some rows). Shared with the tests' secret walk. */
int explore_hidden(const char *table, const char *column);

/* Everything under /admin: the dashboard (GET /admin, templates/dashboard.html),
 * the data explorer (GET /admin/data, templates/explorer.html), the read API
 * both pages are clients of (GET /admin/api/{meta,rows,row,approval,cache,session})
 * and the actions (POST /admin/api/act, explore_act.h). db is the web server's
 * handle; this takes web_db_lock() itself, for the database work only. */
int explore_handle(struct mg_connection *conn, sqlite3 *db);

#endif
