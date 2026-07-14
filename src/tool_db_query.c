#define _POSIX_C_SOURCE 200809L
#include "tool_db_query.h"
#include "util.h"
#include "buf.h"
#include "tool_args.h"
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

static int is_select_only(const char *sql) {
    while (*sql && isspace((unsigned char)*sql)) sql++;
    return (ascii_strncasecmp(sql, "SELECT", 6) == 0 &&
            (sql[6] == '\0' || isspace((unsigned char)sql[6])));
}

char *tool_db_query_handler(const char *arguments, void *user_data) {
    sqlite3 *db = (sqlite3 *)user_data;
    if (!db) return strdup("error: no database connection");

    char *sql = tool_args_str(db, arguments, "sql");
    if (!sql || !sql[0]) {
        free(sql);
        return strdup("error: missing or empty 'sql' field");
    }

    if (!is_select_only(sql)) {
        free(sql);
        return strdup("error: only SELECT queries allowed");
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        char *err = malloc(256);
        if (err) snprintf(err, 256, "error: %s", sqlite3_errmsg(db));
        free(sql);
        return err ? err : strdup("error: prepare failed");
    }
    free(sql);

    int col_count = sqlite3_column_count(stmt);
    Buf b = {0};

    /* Header row */
    for (int i = 0; i < col_count; i++) {
        const char *name = sqlite3_column_name(stmt, i);
        size_t nlen = strlen(name);
        if (i > 0) buf_append_char(&b, '|');
        buf_append(&b, name, nlen);
    }
    buf_append_char(&b, '\n');

    int rows = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && rows < MAX_ROWS) {
        /* skip rows containing encrypted secret values */
        int has_secret = 0;
        for (int i = 0; i < col_count; i++) {
            if (sqlite3_column_type(stmt, i) == SQLITE_TEXT) {
                const char *val = (const char *)sqlite3_column_text(stmt, i);
                if (val && strncmp(val, "enc:", 4) == 0) { has_secret = 1; break; }
            }
        }
        if (has_secret) continue;

        for (int i = 0; i < col_count; i++) {
            const char *val;
            char numbuf[64];
            if (sqlite3_column_type(stmt, i) == SQLITE_NULL) {
                val = "";
            } else if (sqlite3_column_type(stmt, i) == SQLITE_INTEGER) {
                snprintf(numbuf, sizeof(numbuf), "%lld", (long long)sqlite3_column_int64(stmt, i));
                val = numbuf;
            } else if (sqlite3_column_type(stmt, i) == SQLITE_FLOAT) {
                snprintf(numbuf, sizeof(numbuf), "%g", sqlite3_column_double(stmt, i));
                val = numbuf;
            } else {
                val = (const char *)sqlite3_column_text(stmt, i);
                if (!val) val = "";
            }
            size_t vlen = strlen(val);
            if (i > 0) buf_append_char(&b, '|');
            buf_append(&b, val, vlen);
        }
        buf_append_char(&b, '\n');
        rows++;
    }

    sqlite3_finalize(stmt);

    char *out = buf_take(&b);
    if (!out) return strdup("error: OOM");
    return out;
}

/* EXEC_THREAD shim: db_query's "ctx" is just a db handle — use the thread's. */
static char *db_query_thread_run(sqlite3 *db, const char *agent_name,
                                 int64_t session_id, const char *args) {
    (void)agent_name; (void)session_id;
    return tool_db_query_handler(args, db);
}

int tool_db_query_register(ToolRegistry *reg, sqlite3 *db) {
    int rc = tools_register(reg, "db_query",
                          "Execute read-only SQL (SELECT only) against cclaw.db",
                          DB_QUERY_PARAMS_JSON, tool_db_query_handler, db);
    if (rc == 0)
        tools_set_recipe(reg, "db_query",
                         (ToolRecipe){EXEC_THREAD, SBX_NONE, db_query_thread_run});
    return rc;
}
