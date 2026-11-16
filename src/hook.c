#define _POSIX_C_SOURCE 200809L
#include "hook.h"

#include <stdio.h>
#include <string.h>

#include "db.h"
#include "types.h"

/* hooks.input as JSON json_extract can read: promote stores a validated
 * object or NULL, and a row edited by hand into something else reads as
 * "nothing declared" rather than failing every turn. */
#define HOOK_INPUT "(CASE WHEN json_valid(h.input) THEN h.input ELSE '{}' END)"

int hook_next(sqlite3 *db, const char *agent, const char *event, int wait,
              const HookRef *after, HookRef *out) {
    if (!db || !agent || !event || !out) return -1;
    /* Same visibility rule as tools_load_extension_tools: attached + enabled,
     * and published or owned by this agent. Row-value comparison gives the
     * stable order one hook at a time, with no cursor to keep. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT h.extension_name, h.path, h.wait,"
            "       CASE json_extract(" HOOK_INPUT ",'$.history')"
            "         WHEN 'all' THEN -1 WHEN 'none' THEN 0"
            "         ELSE COALESCE(json_extract(" HOOK_INPUT ",'$.history'), 0) END,"
            "       COALESCE(json_extract(" HOOK_INPUT ",'$.history_results'), 1),"
            "       COALESCE(json_extract(" HOOK_INPUT ",'$.max_bytes'), 0)"
            "  FROM hooks h"
            " JOIN agent_extensions ae ON ae.extension_name = h.extension_name"
            " JOIN extensions e ON e.name = h.extension_name"
            " WHERE ae.agent_name = ?1 AND ae.enabled = 1 AND h.enabled = 1"
            "   AND e.enabled = 1 AND h.event = ?2"
            "   AND (e.published = 1 OR e.owner_agent = ?1)"
            "   AND (?3 < 0 OR h.wait = ?3)"
            "   AND (?4 IS NULL OR (h.extension_name, h.path) > (?4, ?5))"
            " ORDER BY h.extension_name, h.path LIMIT 1;",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, event, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, wait);
    if (after) {
        sqlite3_bind_text(st, 4, after->extension, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 5, after->path, -1, SQLITE_STATIC);
    }
    int rc = sqlite3_step(st);
    int found = 0;
    if (rc == SQLITE_ROW) {
        const char *ext = (const char *)sqlite3_column_text(st, 0);
        const char *path = (const char *)sqlite3_column_text(st, 1);
        HookRef h = {0};
        int n1 = snprintf(h.extension, sizeof(h.extension), "%s", ext ? ext : "");
        int n2 = snprintf(h.path, sizeof(h.path), "%s", path ? path : "");
        h.wait = sqlite3_column_int(st, 2);
        h.input.history = sqlite3_column_int(st, 3);
        if (h.input.history < 0) h.input.history = TURN_EDIT_HISTORY_ALL;
        h.input.history_results = sqlite3_column_int(st, 4) != 0;
        int64_t max = sqlite3_column_int64(st, 5);
        h.input.max_bytes = max <= 0 ? TURN_EDIT_INPUT_DEFAULT_BYTES
                          : max > TURN_EDIT_INPUT_MAX_BYTES ? TURN_EDIT_INPUT_MAX_BYTES
                          : (int)max;
        /* A name or path that does not fit names a different hook once cut. */
        if (n1 > 0 && (size_t)n1 < sizeof(h.extension) &&
            n2 > 0 && (size_t)n2 < sizeof(h.path)) {
            *out = h;   /* via a copy: `after` may be `out` */
            found = 1;
        } else {
            rc = SQLITE_ERROR;
        }
    }
    sqlite3_finalize(st);
    if (found) return 1;
    return rc == SQLITE_DONE ? 0 : -1;
}

static int no(char *why, size_t why_len, const char *reason) {
    if (why && why_len) snprintf(why, why_len, "%s", reason);
    return 0;
}

int hook_turn_settled_due(sqlite3 *db, int64_t session_id, const char *agent,
                          char *why, size_t why_len) {
    if (why && why_len) why[0] = '\0';
    if (!db || !agent) return no(why, why_len, "no agent");
    /* Cheapest test first: most agents have no hook, and they pay one
     * indexed lookup per turn for this feature. */
    HookRef h;
    if (hook_next(db, agent, HOOK_EVENT_TURN_SETTLED, -1, NULL, &h) != 1)
        return no(why, why_len, "no hook");
    if (db_scalar_i64(db, "SELECT COALESCE(parent_session_id,-1) > 0"
                          " FROM sessions WHERE id=?1;", session_id, 1))
        return no(why, why_len, "sub-agent session");
    /* The turn must have ended with an answer. An error stop — a provider
     * failure, the iteration or context cutoff — did not complete normally. */
    sqlite3_stmt *st;
    int ended = 0;
    if (sqlite3_prepare_v2(db,
            "SELECT e.type='assistant_message' AND COALESCE(e.stop_reason,0) != ?2"
            " FROM entries e WHERE e.id=(SELECT leaf_id FROM sessions WHERE id=?1)"
            "  AND e.session_id=?1;", -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, session_id);
        sqlite3_bind_int(st, 2, STOP_REASON_ERROR);
        if (sqlite3_step(st) == SQLITE_ROW) ended = sqlite3_column_int(st, 0);
        sqlite3_finalize(st);
    }
    if (!ended) return no(why, why_len, "turn did not end with an answer");
    return 1;
}

int hook_record_outcome(sqlite3 *db, const HookRef *h, const char *event,
                        int failed) {
    if (!db || !h || !event) return 0;
    /* One statement, so two sessions failing the same hook at once still
     * count to three exactly once. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE hooks SET fail_count = CASE WHEN ?4 THEN fail_count + 1 ELSE 0 END"
            " WHERE extension_name=?1 AND event=?2 AND path=?3"
            " RETURNING fail_count;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(st, 1, h->extension, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, event, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, h->path, -1, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, failed ? 1 : 0);
    int count = 0;
    if (sqlite3_step(st) == SQLITE_ROW) count = sqlite3_column_int(st, 0);
    /* RETURNING: step to completion or the write is not finished. */
    while (sqlite3_step(st) == SQLITE_ROW) {}
    sqlite3_finalize(st);
    return failed && count == HOOK_FAIL_NOTIFY;
}
