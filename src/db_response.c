#define _POSIX_C_SOURCE 200809L
#include "db_response.h"
#include "db.h"
#include "llm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* T296: SQL response ingress.
 * Tool call arguments validated with json() — invalid args stored verbatim. */

int db_tool_call_complete(sqlite3 *db, int64_t entry_id, const char *call_id) {
    if (!db || !call_id) return -1;

    const char *sql =
        "UPDATE tool_calls SET status='done' WHERE entry_id=? AND call_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, entry_id);
    sqlite3_bind_text(stmt, 2, call_id, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int db_tool_call_complete_with_result(sqlite3 *db, int64_t entry_id,
                                      const char *call_id, int64_t result_entry_id) {
    if (!db || !call_id) return -1;
    const char *sql =
        "UPDATE tool_calls SET status='done', result_entry_id=?"
        " WHERE entry_id=? AND call_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, result_entry_id);
    sqlite3_bind_int64(stmt, 2, entry_id);
    sqlite3_bind_text(stmt, 3, call_id, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(stmt) == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int db_ingest_typed(sqlite3 *db, int64_t session_id, int64_t turn_id,
                    const char *model, const char *content, const char *reasoning,
                    const char *finish_reason,
                    int usage_in, int usage_out, int64_t cost_nano,
                    const char *const *tc_ids, const char *const *tc_names,
                    const char *const *tc_args, int tc_count,
                    TypedIngestResult *out) {
    if (!db) return -1;
    if (out) memset(out, 0, sizeof(*out));

    StopReason stop = map_stop_reason(finish_reason);
    /* Gemini: STOP with tool calls means tool_use */
    if (stop == STOP_REASON_NONE && tc_count > 0)
        stop = STOP_REASON_TOOL_USE;

    int part = 0;

    /* 1. Reasoning entry (if present) */
    if (reasoning && reasoning[0]) {
        entry_append_typed(db, session_id, turn_id, "reasoning", part++,
                           reasoning, NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    }

    /* 2. Assistant message entry */
    int64_t asst_id = entry_append_typed(db, session_id, turn_id, "assistant_message", part++,
                                         content, NULL, NULL, 0, stop, model,
                                         usage_in, usage_out, cost_nano);
    if (asst_id < 0) return -1;
    if (out) out->assistant_entry_id = asst_id;

    /* 3. Tool call entries + tool_calls table rows */
    if (tc_count > 0 && out) {
        out->tc_entry_ids = malloc((size_t)tc_count * sizeof(int64_t));
        out->tc_count = tc_count;
    }

    const char *tc_ins_sql =
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, arguments)"
        " VALUES(?1,?2,?3,?4,CASE WHEN json_valid(?5) THEN json(?5) ELSE ?5 END);";
    sqlite3_stmt *tc_stmt = NULL;
    if (tc_count > 0)
        sqlite3_prepare_v2(db, tc_ins_sql, -1, &tc_stmt, NULL);

    for (int i = 0; i < tc_count; i++) {
        const char *args_val = (tc_args[i] && tc_args[i][0]) ? tc_args[i] : "{}";
        int64_t tc_entry = entry_append_typed(db, session_id, turn_id, "tool_call", part++,
                                              args_val, tc_ids[i], tc_names[i],
                                              0, STOP_REASON_NONE, NULL, 0, 0, 0);
        if (out && out->tc_entry_ids) out->tc_entry_ids[i] = tc_entry;

        if (tc_stmt && tc_entry > 0) {
            sqlite3_bind_int64(tc_stmt, 1, session_id);
            sqlite3_bind_int64(tc_stmt, 2, tc_entry);
            sqlite3_bind_text(tc_stmt, 3, tc_ids[i] ? tc_ids[i] : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(tc_stmt, 4, tc_names[i] ? tc_names[i] : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(tc_stmt, 5, args_val, -1, SQLITE_STATIC);
            sqlite3_step(tc_stmt);
            sqlite3_reset(tc_stmt);
        }
    }
    if (tc_stmt) sqlite3_finalize(tc_stmt);

    return 0;
}
