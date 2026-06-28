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

/* Scalar columns, identical order for both providers:
 *   0 content   1 finish_reason   2 reasoning   3 prompt_tokens
 *   4 completion_tokens   5 total_tokens   6 cost(real)   7 array_length */
static const char SCALAR_OPENAI[] =
    "SELECT"
    " json_extract(?1,'$.choices[0].message.content'),"
    " json_extract(?1,'$.choices[0].finish_reason'),"
    " COALESCE(json_extract(?1,'$.choices[0].message.reasoning'),"
    "          json_extract(?1,'$.choices[0].message.reasoning_content'),"
    "          json_extract(?1,'$.choices[0].message.reasoning_text')),"
    " json_extract(?1,'$.usage.prompt_tokens'),"
    " json_extract(?1,'$.usage.completion_tokens'),"
    " json_extract(?1,'$.usage.total_tokens'),"
    " COALESCE(json_extract(?1,'$.usage.cost'), json_extract(?1,'$.usage.total_cost')),"
    " json_array_length(?1,'$.choices')";

static const char SCALAR_GEMINI[] =
    "SELECT"
    " (SELECT group_concat(json_extract(value,'$.text'), char(10))"
    "    FROM json_each(?1,'$.candidates[0].content.parts')"
    "    WHERE json_extract(value,'$.text') IS NOT NULL),"
    " CASE json_extract(?1,'$.candidates[0].finishReason')"
    "   WHEN 'STOP' THEN 'stop' WHEN 'MAX_TOKENS' THEN 'length'"
    "   WHEN 'SAFETY' THEN 'content_filter' WHEN 'RECITATION' THEN 'content_filter'"
    "   ELSE json_extract(?1,'$.candidates[0].finishReason') END,"
    " NULL,"
    " json_extract(?1,'$.usageMetadata.promptTokenCount'),"
    " json_extract(?1,'$.usageMetadata.candidatesTokenCount'),"
    " json_extract(?1,'$.usageMetadata.totalTokenCount'),"
    " NULL,"
    " json_array_length(?1,'$.candidates')";

/* Tool-call rows, columns: 0 id (NULL for Gemini), 1 name, 2 arguments. */
static const char TC_OPENAI[] =
    "SELECT json_extract(value,'$.id'),"
    " json_extract(value,'$.function.name'),"
    " json_extract(value,'$.function.arguments')"
    " FROM json_each(?1,'$.choices[0].message.tool_calls')";

static const char TC_GEMINI[] =
    "SELECT NULL,"
    " json_extract(value,'$.functionCall.name'),"
    " json_extract(value,'$.functionCall.args')"
    " FROM json_each(?1,'$.candidates[0].content.parts')"
    " WHERE json_extract(value,'$.functionCall') IS NOT NULL";

/* Default ring-buffer size for llm_responses; overridden by the config key
 * 'llm_response_archive_max'. A negative cap disables pruning (keep all rows). */
#define LLM_RESPONSE_ARCHIVE_MAX 500

/* Archive one raw response for forensics. When is_jsonb, body/blen is the
 * parsed JSONB blob (and we can pull the provider's $.id out of it); otherwise
 * body is NUL-terminated text that wasn't valid JSON. Best-effort: errors here
 * never affect ingest. */
static void archive_store(sqlite3 *db, int64_t session_id, int64_t turn_id,
                          const char *model, const char *status,
                          const void *body, int blen, int is_jsonb) {
    /* Retention cap (config 'llm_response_archive_max'):
     *   > 0  keep the most recent N rows
     *   == 0 archiving off — write nothing
     *   < 0  keep everything (no pruning) */
    int cap = LLM_RESPONSE_ARCHIVE_MAX;
    sqlite3_stmt *c;
    if (sqlite3_prepare_v2(db,
            "SELECT CAST(value AS INTEGER) FROM config WHERE key='llm_response_archive_max'",
            -1, &c, NULL) == SQLITE_OK) {
        if (sqlite3_step(c) == SQLITE_ROW && sqlite3_column_type(c, 0) != SQLITE_NULL)
            cap = sqlite3_column_int(c, 0);
        sqlite3_finalize(c);
    }
    if (cap == 0) return;   /* archiving disabled — skip the insert entirely */

    const char *sql =
        "INSERT INTO llm_responses(session_id,turn_id,model,status,provider_id,body)"
        " VALUES(?1,?2,?3,?4,"
        "  CASE WHEN ?5 THEN COALESCE(json_extract(?6,'$.id'),"
        "                             json_extract(?6,'$.responseId')) END, ?6);";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, session_id);
        sqlite3_bind_int64(s, 2, turn_id);
        sqlite3_bind_text(s, 3, model ? model : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, status, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 5, is_jsonb);
        if (is_jsonb) sqlite3_bind_blob(s, 6, body, blen, SQLITE_STATIC);
        else sqlite3_bind_text(s, 6, (const char *)body, -1, SQLITE_STATIC);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    if (cap > 0) {
        char prune[160];
        snprintf(prune, sizeof(prune),
            "DELETE FROM llm_responses WHERE id <= (SELECT MAX(id)-%d FROM llm_responses);", cap);
        sqlite3_exec(db, prune, NULL, NULL, NULL);
    }
}

/* Public entry: archive a raw response body (any HTTP outcome) given as text.
 * Parses to JSONB when valid, stores raw text otherwise. Used for non-2xx /
 * network errors that never reach db_ingest_response. */
void db_archive_response(sqlite3 *db, int64_t session_id, int64_t turn_id,
                         const char *model, const char *status, const char *body) {
    if (!db || !status) return;
    if (!body) body = "";
    sqlite3_stmt *j;
    if (sqlite3_prepare_v2(db, "SELECT jsonb(?1)", -1, &j, NULL) == SQLITE_OK) {
        sqlite3_bind_text(j, 1, body, -1, SQLITE_STATIC);
        if (sqlite3_step(j) == SQLITE_ROW) {
            archive_store(db, session_id, turn_id, model, status,
                          sqlite3_column_blob(j, 0), sqlite3_column_bytes(j, 0), 1);
            sqlite3_finalize(j);
            return;
        }
        sqlite3_finalize(j);
    }
    archive_store(db, session_id, turn_id, model, status, body, -1, 0);
}

LlmRespStatus db_ingest_response(sqlite3 *db, int64_t session_id, int64_t turn_id,
                                 const char *model, EndpointType ep,
                                 const char *body, TypedIngestResult *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!db || !body) return LLM_RESP_MALFORMED;

    int gemini = (ep == ENDPOINT_GEMINI);

    /* ── Parse the body once into a JSONB blob. Every read below binds the
     * blob (no re-parse), and the same blob is archived. Keep this statement
     * open: the blob lives in its result memory until it is finalized. ── */
    sqlite3_stmt *j;
    if (sqlite3_prepare_v2(db, "SELECT jsonb(?1)", -1, &j, NULL) != SQLITE_OK)
        return LLM_RESP_MALFORMED;
    sqlite3_bind_text(j, 1, body, -1, SQLITE_STATIC);
    if (sqlite3_step(j) != SQLITE_ROW) {
        /* Not valid JSON at all — archive the raw text for forensics. */
        sqlite3_finalize(j);
        archive_store(db, session_id, turn_id, model, "malformed", body, -1, 0);
        return LLM_RESP_MALFORMED;
    }
    const void *blob = sqlite3_column_blob(j, 0);
    int blen = sqlite3_column_bytes(j, 0);

    /* ── Scalar fields: bind the blob, hold the statement open so the
     * extracted column pointers stay valid through the entry inserts. ── */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, gemini ? SCALAR_GEMINI : SCALAR_OPENAI, -1, &s, NULL) != SQLITE_OK) {
        sqlite3_finalize(j);
        return LLM_RESP_MALFORMED;
    }
    sqlite3_bind_blob(s, 1, blob, blen, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        archive_store(db, session_id, turn_id, model, "malformed", blob, blen, 1);
        sqlite3_finalize(j);
        return LLM_RESP_MALFORMED;
    }

    /* Shape check: choices/candidates array present and non-empty. */
    if (sqlite3_column_type(s, 7) == SQLITE_NULL || sqlite3_column_int(s, 7) == 0) {
        sqlite3_finalize(s);
        archive_store(db, session_id, turn_id, model, "malformed", blob, blen, 1);
        sqlite3_finalize(j);
        return LLM_RESP_MALFORMED;
    }

    const char *content   = (const char *)sqlite3_column_text(s, 0);
    const char *finish    = (const char *)sqlite3_column_text(s, 1);
    const char *reasoning = (const char *)sqlite3_column_text(s, 2);
    int prompt_tokens     = (sqlite3_column_type(s, 3) == SQLITE_INTEGER) ? sqlite3_column_int(s, 3) : 0;
    int completion_tokens = (sqlite3_column_type(s, 4) == SQLITE_INTEGER) ? sqlite3_column_int(s, 4) : 0;
    int total_tokens      = (sqlite3_column_type(s, 5) == SQLITE_INTEGER)
                                ? sqlite3_column_int(s, 5)
                                : prompt_tokens + completion_tokens;
    int64_t cost_nano = 0;
    if (sqlite3_column_type(s, 6) != SQLITE_NULL)
        cost_nano = (int64_t)(sqlite3_column_double(s, 6) * 1e9 + 0.5);

    /* ── Count tool calls first (need the count to set the assistant stop
     * reason before inserting it, and to keep a tool-call response off the
     * empty-stop path), then rewind for the insert pass. ── */
    sqlite3_stmt *tc = NULL;
    int tc_count = 0;
    if (sqlite3_prepare_v2(db, gemini ? TC_GEMINI : TC_OPENAI, -1, &tc, NULL) == SQLITE_OK) {
        sqlite3_bind_blob(tc, 1, blob, blen, SQLITE_STATIC);
        while (sqlite3_step(tc) == SQLITE_ROW) tc_count++;
        sqlite3_reset(tc);
    }

    /* Zero-usage empty stop (provider glitch): no entries, retry next model. */
    if (tc_count == 0 && total_tokens == 0 && (!content || !content[0]) &&
        finish && strcmp(finish, "stop") == 0) {
        sqlite3_finalize(s);
        if (tc) sqlite3_finalize(tc);
        archive_store(db, session_id, turn_id, model, "empty", blob, blen, 1);
        sqlite3_finalize(j);
        return LLM_RESP_EMPTY;
    }

    /* Well-formed — archive before flattening into entries. */
    archive_store(db, session_id, turn_id, model, "ok", blob, blen, 1);

    StopReason stop = map_stop_reason(finish);
    if (tc_count > 0 && stop != STOP_REASON_TOOL_USE)
        stop = STOP_REASON_TOOL_USE;

    int part = 0;

    /* Reasoning entry (scalar pointers still valid — s untouched). */
    if (reasoning && reasoning[0])
        entry_append_typed(db, session_id, turn_id, "reasoning", part++,
                           reasoning, NULL, NULL, 0, STOP_REASON_NONE, NULL, 0, 0, 0);

    int64_t asst_id = entry_append_typed(db, session_id, turn_id, "assistant_message", part++,
                                         content, NULL, NULL, 0, stop, model,
                                         prompt_tokens, completion_tokens, cost_nano);
    sqlite3_finalize(s);   /* done with content/reasoning pointers */
    if (asst_id < 0) {
        if (tc) sqlite3_finalize(tc);
        sqlite3_finalize(j);
        return LLM_RESP_MALFORMED;
    }

    /* ── Tool call entries + tool_calls table rows ── */
    if (tc && tc_count > 0) {
        const char *tc_ins_sql =
            "INSERT INTO tool_calls(session_id, entry_id, call_id, name, arguments)"
            " VALUES(?1,?2,?3,?4,CASE WHEN json_valid(?5) THEN json(?5) ELSE ?5 END);";
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db, tc_ins_sql, -1, &ins, NULL);

        int idx = 0;
        while (sqlite3_step(tc) == SQLITE_ROW) {
            const char *id   = (const char *)sqlite3_column_text(tc, 0);
            const char *name = (const char *)sqlite3_column_text(tc, 1);
            const char *args = (const char *)sqlite3_column_text(tc, 2);
            char id_buf[32];
            if (!id || !id[0]) {
                snprintf(id_buf, sizeof(id_buf), "call_gemini_%d", idx);
                id = id_buf;
            }
            const char *args_val = (args && args[0]) ? args : "{}";

            int64_t tc_entry = entry_append_typed(db, session_id, turn_id, "tool_call", part++,
                                                  args_val, id, name,
                                                  0, STOP_REASON_NONE, NULL, 0, 0, 0);
            if (tc_entry > 0 && ins) {
                sqlite3_bind_int64(ins, 1, session_id);
                sqlite3_bind_int64(ins, 2, tc_entry);
                sqlite3_bind_text(ins, 3, id, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, name ? name : "", -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 5, args_val, -1, SQLITE_STATIC);
                sqlite3_step(ins);
                sqlite3_reset(ins);
            }
            idx++;
        }
        if (ins) sqlite3_finalize(ins);
    }
    if (tc) sqlite3_finalize(tc);
    sqlite3_finalize(j);   /* blob no longer needed */

    if (out) {
        out->assistant_entry_id = asst_id;
        out->prompt_tokens = prompt_tokens;
        out->completion_tokens = completion_tokens;
        out->cost_nano = cost_nano;
    }
    return LLM_RESP_OK;
}
