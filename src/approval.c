#define _POSIX_C_SOURCE 200809L
#include "approval.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* FNV-1a 32-bit hash → 8 hex chars */
static void fnv1a_hex(const char *data, char *out) {
    uint32_t h = 2166136261u;
    if (data) {
        for (const char *p = data; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 16777619u;
        }
    }
    snprintf(out, 9, "%08x", h);
}

static char *dup_or_null(const unsigned char *s) {
    return s ? strdup((const char *)s) : NULL;
}

static Approval *row_to_approval(sqlite3_stmt *stmt) {
    Approval *a = calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->id = sqlite3_column_int64(stmt, 0);
    a->session_id = sqlite3_column_int64(stmt, 1);
    a->tool_call_id = dup_or_null(sqlite3_column_text(stmt, 2));
    a->tool_name = dup_or_null(sqlite3_column_text(stmt, 3));
    a->action = dup_or_null(sqlite3_column_text(stmt, 4));
    a->scope = dup_or_null(sqlite3_column_text(stmt, 5));
    a->args_json = dup_or_null(sqlite3_column_text(stmt, 6));
    a->args_hash = dup_or_null(sqlite3_column_text(stmt, 7));
    a->state = dup_or_null(sqlite3_column_text(stmt, 8));
    a->decided_via = dup_or_null(sqlite3_column_text(stmt, 9));
    a->requested_at = sqlite3_column_int64(stmt, 10);
    a->expires_at = sqlite3_column_int64(stmt, 11);
    return a;
}

void approval_free(Approval *a) {
    if (!a) return;
    free(a->tool_call_id);
    free(a->tool_name);
    free(a->action);
    free(a->scope);
    free(a->args_json);
    free(a->args_hash);
    free(a->state);
    free(a->decided_via);
    free(a);
}

int64_t approval_create(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                        const char *tool_name, const char *action,
                        const char *scope, const char *args_json) {
    char hash[9];
    fnv1a_hex(args_json, hash);

    const char *sql =
        "INSERT INTO approvals(session_id, tool_call_id, tool_name, action, scope, args_json, args_hash)"
        " VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, tool_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, scope ? scope : "persist", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, args_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, hash, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db);
}

Approval *approval_get_pending(sqlite3 *db, int64_t session_id) {
    const char *sql =
        "SELECT id, session_id, tool_call_id, tool_name, action, scope,"
        " args_json, args_hash, state, decided_via, requested_at, expires_at"
        " FROM approvals WHERE session_id=? AND state='pending' LIMIT 1";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        a = row_to_approval(stmt);
    sqlite3_finalize(stmt);
    return a;
}

Approval *approval_resolve(sqlite3 *db, int64_t id, int approved, const char *decided_via) {
    const char *new_state = approved ? "approved" : "denied";
    const char *sql =
        "UPDATE approvals SET state=?, decided_via=? WHERE id=? AND state='pending'";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, new_state, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, decided_via, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE || sqlite3_changes(db) != 1)
        return NULL;

    /* Return the resolved row */
    const char *sel =
        "SELECT id, session_id, tool_call_id, tool_name, action, scope,"
        " args_json, args_hash, state, decided_via, requested_at, expires_at"
        " FROM approvals WHERE id=?";
    if (sqlite3_prepare_v2(db, sel, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, id);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        a = row_to_approval(stmt);
    sqlite3_finalize(stmt);
    return a;
}

int approval_expire_once(sqlite3 *db, int64_t session_id) {
    const char *sql =
        "UPDATE approvals SET state='expired' WHERE session_id=? AND scope='once' AND state='approved'";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_step(stmt);
    int n = sqlite3_changes(db);
    sqlite3_finalize(stmt);
    return n;
}
