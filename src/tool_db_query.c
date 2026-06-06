#define _POSIX_C_SOURCE 200809L
#include "tool_db_query.h"
#include "tool_parse.h"
#include <cJSON.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *DB_QUERY_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"sql\":{\"type\":\"string\",\"description\":\"Read-only SQL query (SELECT only)\"}"
    "},\"required\":[\"sql\"]}";

#define MAX_ROWS 500

/* Check if SQL is a SELECT statement (reject mutations) */
static int is_select_only(const char *sql) {
    /* Skip leading whitespace */
    while (*sql && isspace((unsigned char)*sql)) sql++;
    /* Must start with SELECT (case-insensitive) */
    return (strncasecmp(sql, "SELECT", 6) == 0 &&
            (sql[6] == '\0' || isspace((unsigned char)sql[6])));
}

char *tool_db_query_handler(const char *arguments, void *user_data) {
    sqlite3 *db = (sqlite3 *)user_data;
    if (!db) return strdup("error: no database connection");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *sql = targ_str(&ta, "sql");
    if (!sql || !sql[0]) {
        tool_parse_free(&ta);
        return strdup("error: missing or empty 'sql' field");
    }

    if (!is_select_only(sql)) {
        tool_parse_free(&ta);
        return strdup("error: only SELECT queries allowed");
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        char *err = malloc(256);
        if (err) snprintf(err, 256, "error: %s", sqlite3_errmsg(db));
        tool_parse_free(&ta);
        return err ? err : strdup("error: prepare failed");
    }

    cJSON *result = cJSON_CreateArray();
    int col_count = sqlite3_column_count(stmt);
    int rows = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW && rows < MAX_ROWS) {
        /* V52,T173: skip rows containing encrypted secret values */
        int has_secret = 0;
        for (int i = 0; i < col_count; i++) {
            if (sqlite3_column_type(stmt, i) == SQLITE_TEXT) {
                const char *val = (const char *)sqlite3_column_text(stmt, i);
                if (val && strncmp(val, "enc:", 4) == 0) {
                    has_secret = 1;
                    break;
                }
            }
        }
        if (has_secret) continue;

        cJSON *row = cJSON_CreateObject();
        for (int i = 0; i < col_count; i++) {
            const char *col_name = sqlite3_column_name(stmt, i);
            int col_type = sqlite3_column_type(stmt, i);
            switch (col_type) {
            case SQLITE_INTEGER:
                cJSON_AddNumberToObject(row, col_name, (double)sqlite3_column_int64(stmt, i));
                break;
            case SQLITE_FLOAT:
                cJSON_AddNumberToObject(row, col_name, sqlite3_column_double(stmt, i));
                break;
            case SQLITE_NULL:
                cJSON_AddNullToObject(row, col_name);
                break;
            default: /* TEXT or BLOB */
                cJSON_AddStringToObject(row, col_name,
                    (const char *)sqlite3_column_text(stmt, i));
                break;
            }
        }
        cJSON_AddItemToArray(result, row);
        rows++;
    }

    sqlite3_finalize(stmt);
    tool_parse_free(&ta);

    char *output = cJSON_PrintUnformatted(result);
    cJSON_Delete(result);
    return output ? output : strdup("[]");
}

int tool_db_query_register(ToolRegistry *reg, sqlite3 *db) {
    return tools_register(reg, "db_query",
                          "Execute read-only SQL (SELECT only) against cclaw.db",
                          DB_QUERY_PARAMS_JSON, tool_db_query_handler, db);
}
