#define _POSIX_C_SOURCE 200809L
#include "approval.h"
#include "db.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    a->args_json = dup_or_null(sqlite3_column_text(stmt, 5));
    a->resolve = dup_or_null(sqlite3_column_text(stmt, 6));
    a->state = dup_or_null(sqlite3_column_text(stmt, 7));
    a->decided_via = dup_or_null(sqlite3_column_text(stmt, 8));
    a->requested_at = sqlite3_column_int64(stmt, 9);
    a->expires_at = sqlite3_column_int64(stmt, 10);
    return a;
}

void approval_free(Approval *a) {
    if (!a) return;
    free(a->tool_call_id);
    free(a->tool_name);
    free(a->action);
    free(a->args_json);
    free(a->resolve);
    free(a->state);
    free(a->decided_via);
    free(a);
}

int64_t approval_create(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                        const char *tool_name, const char *action,
                        const char *args_json, const char *resolve) {
    if (!resolve) resolve = "rerun";

    /* Deadline: kv "approval_timeout_sec" or default 3600 */
    int64_t timeout = 3600;
    char *kv = db_kv_get(db, "approval_timeout_sec");
    if (kv) {
        long v = strtol(kv, NULL, 10);
        if (v > 0) timeout = v;
        free(kv);
    }
    int64_t expires_at = (int64_t)time(NULL) + timeout;

    const char *sql =
        "INSERT INTO approvals(session_id, tool_call_id, tool_name, action, args_json, resolve, expires_at)"
        " VALUES(?,?,?,?,?,?,?)";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, tool_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, args_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, resolve, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 7, expires_at);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    int64_t id = sqlite3_last_insert_rowid(db);
    LOG_INFO_("approval create id=%lld session=%lld tool=%s",
             (long long)id, (long long)session_id, tool_name ? tool_name : "?");
    return id;
}

Approval *approval_get_pending(sqlite3 *db, int64_t session_id) {
    const char *sql =
        "SELECT id, session_id, tool_call_id, tool_name, action,"
        " args_json, resolve, state, decided_via, requested_at, expires_at"
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

Approval *approval_get_pending_subtree(sqlite3 *db, int64_t root_session_id) {
    const char *sql =
        "WITH RECURSIVE subtree(id) AS ("
        "  SELECT ?1"
        "  UNION ALL"
        "  SELECT s.id FROM sessions s JOIN subtree t ON s.parent_session_id = t.id"
        ")"
        " SELECT a.id, a.session_id, a.tool_call_id, a.tool_name, a.action,"
        " a.args_json, a.resolve, a.state, a.decided_via, a.requested_at, a.expires_at"
        " FROM approvals a WHERE a.session_id IN (SELECT id FROM subtree)"
        " AND a.state='pending' ORDER BY a.requested_at ASC, a.id ASC LIMIT 1";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, root_session_id);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        a = row_to_approval(stmt);
    sqlite3_finalize(stmt);
    return a;
}

int approval_session_in_subtree(sqlite3 *db, int64_t root_session_id, int64_t session_id) {
    if (session_id == root_session_id) return 1;
    const char *sql =
        "WITH RECURSIVE subtree(id) AS ("
        "  SELECT ?1"
        "  UNION ALL"
        "  SELECT s.id FROM sessions s JOIN subtree t ON s.parent_session_id = t.id"
        ")"
        " SELECT 1 FROM subtree WHERE id=?2 LIMIT 1";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(stmt, 1, root_session_id);
    sqlite3_bind_int64(stmt, 2, session_id);
    int found = (sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);
    return found;
}

Approval *approval_get_for_tool_call(sqlite3 *db, int64_t session_id,
                                     const char *tool_call_id) {
    if (!tool_call_id) return NULL;
    /* Scoped to (session_id, tool_call_id): tool_call_ids come from the model
     * and are not globally unique, so a bare tool_call_id match would let a
     * reused id collide with another session's approved row. */
    const char *sql =
        "SELECT id, session_id, tool_call_id, tool_name, action,"
        " args_json, resolve, state, decided_via, requested_at, expires_at"
        " FROM approvals WHERE session_id=? AND tool_call_id=? ORDER BY id DESC LIMIT 1";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, tool_call_id, -1, SQLITE_STATIC);
    Approval *a = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        a = row_to_approval(stmt);
    sqlite3_finalize(stmt);
    return a;
}

int approval_consume(sqlite3 *db, int64_t id) {
    /* Terminal transition approved → consumed. Makes a "once" approval
     * single-use: the gate green-lights only state='approved', so once the
     * frozen call has run, a replayed tool_call_id can no longer match it. */
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "UPDATE approvals SET state='consumed' WHERE id=? AND state='approved'",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) == 1) ? 0 : -1;
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
    LOG_INFO_("approval resolve id=%lld decision=%s",
             (long long)id, new_state);

    /* Return the resolved row */
    const char *sel =
        "SELECT id, session_id, tool_call_id, tool_name, action,"
        " args_json, resolve, state, decided_via, requested_at, expires_at"
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



static int64_t *drain_ids(sqlite3_stmt *stmt, int *out_count) {
    int cap = 0, n = 0;
    int64_t *ids = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (n >= cap) {
            cap = cap ? cap * 2 : 8;
            int64_t *tmp = realloc(ids, (size_t)cap * sizeof(*ids));
            if (!tmp) {
                free(ids);
                sqlite3_finalize(stmt);
                *out_count = 0;
                return NULL;
            }
            ids = tmp;
        }
        ids[n++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    *out_count = n;
    return ids;
}

/* Owner filter shared by the sweeps: the approval's session is owned by `me`,
 * unowned, or dead-owned (owner not in the live registry). With `me` bound NULL
 * the `= ?me` arm is never true, reducing to "unowned or dead-owned". */
static void bind_me(sqlite3_stmt *stmt, int idx, const char *me) {
    if (me && me[0]) sqlite3_bind_text(stmt, idx, me, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, idx);
}

int64_t *approval_list_expired(sqlite3 *db, const char *me, int *out_count) {
    *out_count = 0;
    const char *sql =
        "SELECT a.id FROM approvals a JOIN sessions s ON s.id = a.session_id"
        " WHERE a.state='pending'"
        "   AND a.expires_at IS NOT NULL AND a.expires_at < unixepoch()"
        "   AND (s.owner_instance IS NULL OR s.owner_instance = ?1"
        "        OR s.owner_instance NOT IN (SELECT instance_id FROM processes));";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    bind_me(stmt, 1, me);
    return drain_ids(stmt, out_count);
}

int64_t *approval_list_block_due(sqlite3 *db, int block_sec, const char *me, int *out_count) {
    *out_count = 0;
    const char *sql =
        "SELECT a.id FROM approvals a JOIN sessions s ON s.id = a.session_id"
        " WHERE a.state='pending' AND s.state='awaiting_approval'"
        "   AND a.requested_at + ?1 < unixepoch()"
        "   AND (s.owner_instance IS NULL OR s.owner_instance = ?2"
        "        OR s.owner_instance NOT IN (SELECT instance_id FROM processes));";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int(stmt, 1, block_sec);
    bind_me(stmt, 2, me);
    return drain_ids(stmt, out_count);
}

int64_t approval_deliver_postwindow(sqlite3 *db, const Approval *a, ApprovalPostWindow outcome) {
    if (!a) return -1;
    const char *what = a->action ? a->action : (a->tool_name ? a->tool_name : "tool");
    char buf[320];
    switch (outcome) {
    case APPROVAL_PW_RERUN_APPROVED:
        snprintf(buf, sizeof(buf),
                 "Approval #%lld for '%s' was approved after the wait window — "
                 "re-issue the call if it's still needed.",
                 (long long)a->id, what);
        break;
    case APPROVAL_PW_RERUN_DENIED:
        snprintf(buf, sizeof(buf), "Approval #%lld for '%s' was denied.",
                 (long long)a->id, what);
        break;
    case APPROVAL_PW_APPLY_GRANTED:
        snprintf(buf, sizeof(buf), "Approval #%lld granted: %s applied.",
                 (long long)a->id, what);
        break;
    case APPROVAL_PW_APPLY_DENIED:
        snprintf(buf, sizeof(buf), "Approval #%lld denied: %s.",
                 (long long)a->id, what);
        break;
    case APPROVAL_PW_EXPIRED:
    default:
        snprintf(buf, sizeof(buf),
                 "Approval #%lld for '%s' expired without a decision.",
                 (long long)a->id, what);
        break;
    }
    return inbox_insert(db, a->session_id, "approval", buf);
}
