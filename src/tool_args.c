#define _POSIX_C_SOURCE 200809L
#include "tool_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Run one single-row extraction query with (args, '$.'||key) bound.
 * Returns the malloc'd text of column 0, or NULL (absent / wrong type /
 * malformed JSON — json_* raise on invalid input, which fails the step). */
static char *extract_text(sqlite3 *db, const char *sql,
                          const char *args, const char *key) {
    if (!db || !args || !key) return NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    char path[128];
    snprintf(path, sizeof(path), "$.%s", key);
    sqlite3_bind_text(st, 1, args, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, path, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

int tool_args_valid_object(sqlite3 *db, const char *args) {
    if (!db || !args) return 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT json_valid(?1) AND json_type(?1)='object'",
            -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, args, -1, SQLITE_STATIC);
    int ok = (sqlite3_step(st) == SQLITE_ROW) && sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return ok;
}

char *tool_args_str(sqlite3 *db, const char *args, const char *key) {
    return extract_text(db,
        "SELECT CASE WHEN json_type(?1,?2)='text' THEN json_extract(?1,?2) END",
        args, key);
}

int tool_args_int(sqlite3 *db, const char *args, const char *key, int def) {
    char *v = extract_text(db,
        "SELECT CASE WHEN json_type(?1,?2) IN ('integer','real')"
        " THEN CAST(json_extract(?1,?2) AS INTEGER) END",
        args, key);
    if (!v) return def;
    int n = atoi(v);
    free(v);
    return n;
}

int tool_args_bool(sqlite3 *db, const char *args, const char *key, int def) {
    char *v = extract_text(db,
        "SELECT CASE WHEN json_type(?1,?2) IN ('true','false')"
        " THEN json_extract(?1,?2) END",
        args, key);
    if (!v) return def;
    int b = (v[0] == '1');
    free(v);
    return b;
}

char *tool_args_json(sqlite3 *db, const char *args, const char *key) {
    return extract_text(db,
        "SELECT CASE WHEN json_type(?1,?2) IN ('object','array')"
        " THEN json_extract(?1,?2) END",
        args, key);
}
