#define _POSIX_C_SOURCE 200809L
#include "db_response.h"
#include "db.h"
#include "llm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* T296: SQL response ingress.
 *
 * Non-streaming: json_extract() pulls fields from raw response JSON.
 * Streaming: parameterized INSERT from accumulated C strings.
 * Tool call arguments validated with json() — invalid args stored verbatim. */

/* Insert tool_calls rows from a JSON tool_calls array string.
 * Returns number inserted, or -1 on error. */
static int insert_tool_calls_from_json(sqlite3 *db, int64_t session_id,
                                       int64_t entry_id, const char *tc_json) {
    if (!tc_json || tc_json[0] == '\0') return 0;

    /* Use json_each to iterate the tool_calls array and INSERT each row.
     * json_valid() validates arguments — fall back to raw string if invalid. */
    const char *sql =
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, arguments)"
        " SELECT ?1, ?2,"
        "   json_extract(value, '$.id'),"
        "   json_extract(value, '$.name'),"
        "   CASE WHEN json_valid(json_extract(value,'$.args'))"
        "     THEN json(json_extract(value,'$.args'))"
        "     ELSE json_extract(value,'$.args') END"
        " FROM json_each(?3);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, entry_id);
    sqlite3_bind_text(stmt, 3, tc_json, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) return -1;
    return sqlite3_changes(db);
}

/* Insert tool_calls rows from parallel arrays (streaming accumulation). */
static int insert_tool_calls_arrays(sqlite3 *db, int64_t session_id,
                                    int64_t entry_id,
                                    const char *const *ids,
                                    const char *const *names,
                                    const char *const *args, int count) {
    if (count <= 0) return 0;

    const char *sql =
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, arguments)"
        " VALUES(?1,?2,?3,?4,"
        "   CASE WHEN json_valid(?5) THEN json(?5) ELSE ?5 END);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    int inserted = 0;
    for (int i = 0; i < count; i++) {
        sqlite3_bind_int64(stmt, 1, session_id);
        sqlite3_bind_int64(stmt, 2, entry_id);
        sqlite3_bind_text(stmt, 3, ids[i] ? ids[i] : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, names[i] ? names[i] : "", -1, SQLITE_STATIC);
        if (args[i])
            sqlite3_bind_text(stmt, 5, args[i], -1, SQLITE_STATIC);
        else
            sqlite3_bind_text(stmt, 5, "{}", -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_DONE) inserted++;
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    return inserted;
}

int db_ingest_response(sqlite3 *db, int64_t session_id, int64_t parent_id,
                       int64_t turn_id, const char *model,
                       const char *response_json, IngestResult *out) {
    if (!db || !response_json) return -1;
    if (out) memset(out, 0, sizeof(*out));

    /* Single SQL to extract fields and INSERT into entries.
     * json_extract paths follow OpenAI chat completion format.
     * tool_calls transformed from OpenAI format to internal format:
     * OpenAI: [{"id","type","function":{"name","arguments"}}]
     * Internal: [{"id","name","args":object}] */
    const char *sql =
        "INSERT INTO entries(session_id, parent_id, turn_id, role, content,"
        "  tool_calls, stop_reason, model, usage_in, usage_out, cost_nano,"
        "  tool_call_count, token_estimate, content_bytes)"
        " SELECT ?1, ?2, ?3, 2," /* role=2 (assistant) */
        "  json_extract(?4, '$.choices[0].message.content'),"
        "  (SELECT json_group_array("
        "    json_object('id',json_extract(tc.value,'$.id'),"
        "      'name',json_extract(tc.value,'$.function.name'),"
        "      'args',json(json_extract(tc.value,'$.function.arguments'))))"
        "   FROM json_each(json_extract(?4,'$.choices[0].message.tool_calls')) tc"
        "   WHERE json_extract(?4,'$.choices[0].message.tool_calls') IS NOT NULL),"
        "  CASE json_extract(?4, '$.choices[0].finish_reason')"
        "    WHEN 'stop' THEN 1 WHEN 'length' THEN 2"
        "    WHEN 'tool_calls' THEN 3 WHEN 'function_call' THEN 3 WHEN 'tool_use' THEN 3"
        "    ELSE 0 END,"
        "  ?5,"
        "  COALESCE(json_extract(?4, '$.usage.prompt_tokens'),0),"
        "  COALESCE(json_extract(?4, '$.usage.completion_tokens'),0),"
        "  CAST(COALESCE(json_extract(?4,'$.usage.cost'),json_extract(?4,'$.usage.total_cost'),0)*1e9 AS INTEGER),"
        "  COALESCE(json_array_length(json_extract(?4, '$.choices[0].message.tool_calls')),0),"
        "  (LENGTH(COALESCE(json_extract(?4,'$.choices[0].message.content'),''))"
        "   + LENGTH(COALESCE(json_extract(?4,'$.choices[0].message.tool_calls'),'')))/4,"
        "  LENGTH(COALESCE(json_extract(?4,'$.choices[0].message.content'),''))"
        "   + LENGTH(COALESCE(json_extract(?4,'$.choices[0].message.tool_calls'),''))"
        " WHERE json_extract(?4, '$.choices[0]') IS NOT NULL;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;

    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, parent_id);
    sqlite3_bind_int64(stmt, 3, turn_id);
    sqlite3_bind_text(stmt, 4, response_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, model ? model : "", -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    int64_t entry_id = sqlite3_last_insert_rowid(db);
    if (entry_id == 0) return -1; /* WHERE clause didn't match */

    /* Update leaf_id */
    session_set_leaf(db, session_id, entry_id);

    /* Extract tool_calls JSON from entries to insert into tool_calls table */
    int tc_count = 0;
    const char *tc_sql = "SELECT tool_calls FROM entries WHERE id=?;";
    sqlite3_stmt *tc_stmt;
    if (sqlite3_prepare_v2(db, tc_sql, -1, &tc_stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(tc_stmt, 1, entry_id);
        if (sqlite3_step(tc_stmt) == SQLITE_ROW) {
            const char *tc_text = (const char *)sqlite3_column_text(tc_stmt, 0);
            if (tc_text && tc_text[0] == '[')
                tc_count = insert_tool_calls_from_json(db, session_id, entry_id, tc_text);
        }
        sqlite3_finalize(tc_stmt);
    }

    if (out) {
        out->entry_id = entry_id;
        out->tool_call_count = tc_count > 0 ? tc_count : 0;
    }
    return 0;
}

int db_ingest_streaming(sqlite3 *db, int64_t session_id, int64_t parent_id,
                        int64_t turn_id, const char *model,
                        const char *content, const char *finish_reason,
                        int usage_in, int usage_out, int64_t cost_nano,
                        const char *const *tc_ids, const char *const *tc_names,
                        const char *const *tc_args, int tc_count,
                        IngestResult *out) {
    if (!db) return -1;
    if (out) memset(out, 0, sizeof(*out));

    /* Build tool_calls JSON array from accumulated arrays */
    char *tc_json = NULL;
    if (tc_count > 0) {
        size_t cap = 256;
        for (int i = 0; i < tc_count; i++) {
            cap += 64 + (tc_ids[i] ? strlen(tc_ids[i]) : 0)
                      + (tc_names[i] ? strlen(tc_names[i]) : 0)
                      + (tc_args[i] ? strlen(tc_args[i]) : 0);
        }
        tc_json = malloc(cap);
        if (!tc_json) return -1;

        /* Use SQLite json_object() to build each element with proper escaping */
        sqlite3_stmt *bld;
        int pos = 0;
        tc_json[pos++] = '[';
        for (int i = 0; i < tc_count; i++) {
            if (i > 0) tc_json[pos++] = ',';
            /* Use a helper query to produce one valid JSON object per call */
            const char *one_sql =
                "SELECT json_object('id',?1,'name',?2,'args',"
                "  CASE WHEN json_valid(?3) THEN json(?3) ELSE ?3 END);";
            if (sqlite3_prepare_v2(db, one_sql, -1, &bld, NULL) == SQLITE_OK) {
                sqlite3_bind_text(bld, 1, tc_ids[i] ? tc_ids[i] : "", -1, SQLITE_STATIC);
                sqlite3_bind_text(bld, 2, tc_names[i] ? tc_names[i] : "", -1, SQLITE_STATIC);
                sqlite3_bind_text(bld, 3, tc_args[i] ? tc_args[i] : "{}", -1, SQLITE_STATIC);
                if (sqlite3_step(bld) == SQLITE_ROW) {
                    const char *obj = (const char *)sqlite3_column_text(bld, 0);
                    if (obj) {
                        size_t olen = strlen(obj);
                        if ((size_t)pos + olen + 2 > cap) {
                            cap = (size_t)pos + olen + 256;
                            tc_json = realloc(tc_json, cap);
                        }
                        memcpy(tc_json + pos, obj, olen);
                        pos += (int)olen;
                    }
                }
                sqlite3_finalize(bld);
            }
        }
        tc_json[pos++] = ']';
        tc_json[pos] = '\0';
    }

    /* Map finish_reason → stop_reason int */
    int stop_reason = 0;
    if (finish_reason) {
        if (strcmp(finish_reason, "stop") == 0) stop_reason = 1;
        else if (strcmp(finish_reason, "length") == 0) stop_reason = 2;
        else if (strcmp(finish_reason, "tool_calls") == 0 ||
                 strcmp(finish_reason, "function_call") == 0 ||
                 strcmp(finish_reason, "tool_use") == 0) stop_reason = 3;
    }

    /* Compute estimates */
    int content_bytes = (content ? (int)strlen(content) : 0)
                      + (tc_json ? (int)strlen(tc_json) : 0);
    int token_estimate = content_bytes / 4;

    /* INSERT into entries */
    const char *sql =
        "INSERT INTO entries(session_id, parent_id, turn_id, role, content,"
        "  tool_calls, stop_reason, model, usage_in, usage_out, cost_nano,"
        "  tool_call_count, token_estimate, content_bytes)"
        " VALUES(?1,?2,?3,2,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        free(tc_json);
        return -1;
    }

    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_int64(stmt, 2, parent_id);
    sqlite3_bind_int64(stmt, 3, turn_id);
    if (content && content[0])
        sqlite3_bind_text(stmt, 4, content, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 4);
    if (tc_json)
        sqlite3_bind_text(stmt, 5, tc_json, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(stmt, 5);
    sqlite3_bind_int(stmt, 6, stop_reason);
    sqlite3_bind_text(stmt, 7, model ? model : "", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 8, usage_in);
    sqlite3_bind_int(stmt, 9, usage_out);
    sqlite3_bind_int64(stmt, 10, cost_nano);
    sqlite3_bind_int(stmt, 11, tc_count);
    sqlite3_bind_int(stmt, 12, token_estimate);
    sqlite3_bind_int(stmt, 13, content_bytes);

    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    free(tc_json);
    if (rc != SQLITE_DONE) return -1;

    int64_t entry_id = sqlite3_last_insert_rowid(db);
    session_set_leaf(db, session_id, entry_id);

    /* Insert into tool_calls table */
    int inserted = 0;
    if (tc_count > 0)
        inserted = insert_tool_calls_arrays(db, session_id, entry_id,
                                            tc_ids, tc_names, tc_args, tc_count);

    if (out) {
        out->entry_id = entry_id;
        out->tool_call_count = inserted > 0 ? inserted : 0;
    }
    return 0;
}

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

    /* Map finish_reason → StopReason */
    StopReason stop = STOP_REASON_NONE;
    if (finish_reason) {
        if (strcmp(finish_reason, "stop") == 0 || strcmp(finish_reason, "end_turn") == 0)
            stop = STOP_REASON_STOP;
        else if (strcmp(finish_reason, "length") == 0 || strcmp(finish_reason, "MAX_TOKENS") == 0)
            stop = STOP_REASON_LENGTH;
        else if (strcmp(finish_reason, "tool_calls") == 0 ||
                 strcmp(finish_reason, "tool_use") == 0 ||
                 strcmp(finish_reason, "function_call") == 0 ||
                 strcmp(finish_reason, "STOP") == 0)
            stop = STOP_REASON_TOOL_USE;
    }
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
