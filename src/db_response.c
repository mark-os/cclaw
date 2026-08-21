#define _POSIX_C_SOURCE 200809L
#include "db_response.h"
#include "config_registry.h"
#include "db.h"
#include "llm.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* SQL response ingress.
 * Tool call arguments validated with json() — stored in entries.content as
 * normalized text (single source of truth). Invalid args get an immediate error
 * tool_result. tool_calls table holds only workflow state, no arguments. */

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
 *   4 completion_tokens   5 total_tokens   6 cost(real)   7 array_length
 *   8 reasoning replay blob (JSON text, NULL when the response carries none)
 *   9 cached_tokens   10 cache_write_tokens   11 reasoning_tokens
 *
 * Column 8 is the verbatim wire artifact the provider requires back on the
 * next request (specs: OpenRouter reasoning_details — order and every field
 * matter, it is how Gemini's thoughtSignature round-trips through the
 * OpenAI-compat envelope; Gemini-native thoughtSignature per part). Stored
 * unmodified in entries.reasoning_meta.blob; see replay in llm_payload.c.
 * Columns 9-11 are NULL on providers that don't report them; nothing depends
 * on them (absent == pre-M4 behavior everywhere). cached_tokens is a SUBSET of
 * prompt_tokens on every provider supported today (cache-inclusive wire
 * semantics) — a cache-exclusive provider would have to be normalized here. */
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
    " json_array_length(?1,'$.choices'),"
    " CASE WHEN json_array_length(?1,'$.choices[0].message.reasoning_details') > 0"
    "   THEN json_extract(?1,'$.choices[0].message.reasoning_details') END,"
    /* DeepSeek-direct spells the cache-read count prompt_cache_hit_tokens;
     * OpenAI/OpenRouter nest it under prompt_tokens_details. */
    " COALESCE(json_extract(?1,'$.usage.prompt_tokens_details.cached_tokens'),"
    "          json_extract(?1,'$.usage.prompt_cache_hit_tokens')),"
    " json_extract(?1,'$.usage.prompt_tokens_details.cache_write_tokens'),"
    " json_extract(?1,'$.usage.completion_tokens_details.reasoning_tokens')";

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
    /* thoughtsTokenCount is a sibling of candidatesTokenCount, not a part of
     * it — summing them is what makes Gemini's completion count comparable to
     * the OpenAI-compat one (pre-M4 we stored only candidates and undercounted
     * every thinking response). */
    " COALESCE(json_extract(?1,'$.usageMetadata.candidatesTokenCount'),0)"
    "   + COALESCE(json_extract(?1,'$.usageMetadata.thoughtsTokenCount'),0),"
    " json_extract(?1,'$.usageMetadata.totalTokenCount'),"
    " NULL,"
    " json_array_length(?1,'$.candidates'),"
    /* One row per part that carried a thoughtSignature, tagged with the
     * functionCall name it rode on — that name is what replay matches, since
     * part_index in entries counts only the parts we kept. */
    " (SELECT NULLIF(json_group_array(json_object("
    "     'fn', json_extract(value,'$.functionCall.name'),"
    "     'sig', json_extract(value,'$.thoughtSignature'))), '[]')"
    "    FROM json_each(?1,'$.candidates[0].content.parts')"
    "    WHERE json_extract(value,'$.thoughtSignature') IS NOT NULL),"
    " json_extract(?1,'$.usageMetadata.cachedContentTokenCount'),"
    " NULL,"
    " json_extract(?1,'$.usageMetadata.thoughtsTokenCount')";

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

/* Tag a freshly inserted type='reasoning' entry with everything replay needs:
 * who produced it (provider+model — a model switch strips the replay, pi's
 * rule) and the verbatim wire artifact under $.blob. The blob is always where
 * replay reads from, never entries.content: the content half is display-only
 * and the save_reasoning config discards it by default, while the wire
 * requirement holds regardless of what an operator wants to *see*.
 *   gemini_parts      blob = [{fn, sig}, …]  (native thoughtSignature)
 *   reasoning_details blob = the provider's array, byte-for-byte
 *   reasoning_content blob = the bare string (DeepSeek-style)
 * Best-effort: the entry is already durable, a failure here just means no
 * replay for that iteration. */
static void reasoning_meta_set(sqlite3 *db, int64_t entry_id, int gemini,
                               const char *model, const char *blob_json,
                               const char *text) {
    const char *fmt = gemini ? "gemini_parts"
                             : (blob_json ? "reasoning_details" : "reasoning_content");
    if (gemini && !blob_json) return;   /* nothing replayable on this path */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE entries SET reasoning_meta = json_object("
            "  'provider', ?2, 'model', ?3, 'format', ?4,"
            "  'blob', CASE WHEN ?5 IS NOT NULL THEN json(?5) ELSE ?6 END)"
            " WHERE id = ?1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(s, 1, entry_id);
    sqlite3_bind_text(s, 2, gemini ? "gemini" : "openai", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, model ? model : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 4, fmt, -1, SQLITE_STATIC);
    if (blob_json) sqlite3_bind_text(s, 5, blob_json, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 5);
    if (text) sqlite3_bind_text(s, 6, text, -1, SQLITE_STATIC);
    else sqlite3_bind_null(s, 6);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* Archive one raw response for forensics. When is_jsonb, body/blen is the
 * parsed JSONB blob (and we can pull the provider's $.id out of it); otherwise
 * body is NUL-terminated text that wasn't valid JSON. Best-effort: errors here
 * never affect ingest. usage (may be NULL) carries the extracted usage block —
 * only the success path has one; failures archive with NULL usage columns. */
typedef struct {
    int prompt_tokens, completion_tokens;
    int cached_tokens, cache_write_tokens, reasoning_tokens;  /* <0 = absent */
    double cost; int has_cost;
} RespUsage;

static void archive_store(sqlite3 *db, int64_t session_id, int64_t iteration_id,
                          const char *model, const char *status,
                          const void *body, int blen, int is_jsonb,
                          const char *request_body, const RespUsage *usage) {
    /* Retention cap (config 'llm_response_archive_max'):
     *   > 0  keep the most recent N 'ok' rows and the most recent N failures
     *   == 0 archiving off — write nothing
     *   < 0  keep everything (no pruning) */
    int cap = config_default_int("llm_response_archive_max");
    sqlite3_stmt *c;
    if (sqlite3_prepare_v2(db,
            "SELECT CAST(COALESCE(value, default_value) AS INTEGER) FROM config"
            " WHERE key='llm_response_archive_max'",
            -1, &c, NULL) == SQLITE_OK) {
        if (sqlite3_step(c) == SQLITE_ROW && sqlite3_column_type(c, 0) != SQLITE_NULL)
            cap = sqlite3_column_int(c, 0);
        sqlite3_finalize(c);
    }
    if (cap == 0) return;   /* archiving disabled — skip the insert entirely */

    const char *sql =
        "INSERT INTO llm_responses(session_id,iteration_id,model,status,provider_id,body,"
        "  request_body,cached_tokens,cache_write_tokens,reasoning_tokens,cost)"
        " VALUES(?1,?2,?3,?4,"
        "  CASE WHEN ?5 THEN COALESCE(json_extract(?6,'$.id'),"
        "                             json_extract(?6,'$.responseId')) END, ?6,"
        "  CASE WHEN ?7 IS NOT NULL THEN jsonb(?7) END,?8,?9,?10,?11);";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(s, 1, session_id);
        sqlite3_bind_int64(s, 2, iteration_id);
        sqlite3_bind_text(s, 3, model ? model : "", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 4, status, -1, SQLITE_STATIC);
        sqlite3_bind_int(s, 5, is_jsonb);
        if (is_jsonb) sqlite3_bind_blob(s, 6, body, blen, SQLITE_STATIC);
        else sqlite3_bind_text(s, 6, (const char *)body, -1, SQLITE_STATIC);
        if (request_body) sqlite3_bind_text(s, 7, request_body, -1, SQLITE_STATIC);
        else sqlite3_bind_null(s, 7);
        int u[3] = { usage ? usage->cached_tokens : -1,
                     usage ? usage->cache_write_tokens : -1,
                     usage ? usage->reasoning_tokens : -1 };
        for (int i = 0; i < 3; i++) {
            if (u[i] >= 0) sqlite3_bind_int(s, 8 + i, u[i]);
            else sqlite3_bind_null(s, 8 + i);
        }
        if (usage && usage->has_cost) sqlite3_bind_double(s, 11, usage->cost);
        else sqlite3_bind_null(s, 11);
        sqlite3_step(s);
        sqlite3_finalize(s);
    }

    if (cap > 0) {
        /* Prune successes and failures independently: failure rows are what
         * error entries cite ("[resp #N]") and are rare — a busy session's
         * steady stream of 'ok' rows must not push them out before the
         * operator gets to look. */
        char prune[512];
        snprintf(prune, sizeof(prune),
            "DELETE FROM llm_responses WHERE status='ok' AND id NOT IN"
            " (SELECT id FROM llm_responses WHERE status='ok' ORDER BY id DESC LIMIT %d);"
            "DELETE FROM llm_responses WHERE status!='ok' AND id NOT IN"
            " (SELECT id FROM llm_responses WHERE status!='ok' ORDER BY id DESC LIMIT %d);",
            cap, cap);
        sqlite3_exec(db, prune, NULL, NULL, NULL);
    }
}

/* Public entry: archive a raw response body (any HTTP outcome) given as text.
 * Parses to JSONB when valid, stores raw text otherwise. Used for non-2xx /
 * network errors that never reach db_ingest_response. */
void db_archive_response(sqlite3 *db, int64_t session_id, int64_t iteration_id,
                         const char *model, const char *status, const char *body,
                         const char *request_body) {
    if (!db || !status) return;
    if (!body) body = "";
    sqlite3_stmt *j;
    if (sqlite3_prepare_v2(db, "SELECT jsonb(?1)", -1, &j, NULL) == SQLITE_OK) {
        sqlite3_bind_text(j, 1, body, -1, SQLITE_STATIC);
        if (sqlite3_step(j) == SQLITE_ROW) {
            archive_store(db, session_id, iteration_id, model, status,
                          sqlite3_column_blob(j, 0), sqlite3_column_bytes(j, 0), 1,
                          request_body, NULL);
            sqlite3_finalize(j);
            return;
        }
        sqlite3_finalize(j);
    }
    archive_store(db, session_id, iteration_id, model, status, body, -1, 0, request_body, NULL);
}

LlmRespStatus db_ingest_response(sqlite3 *db, int64_t session_id, int64_t iteration_id,
                                 const char *model, EndpointType ep,
                                 const char *body, const char *request_body,
                                 int save_reasoning, TypedIngestResult *out) {
    if (out) memset(out, 0, sizeof(*out));
    if (!db || !body) return LLM_RESP_MALFORMED;

    int gemini = (ep == ENDPOINT_GEMINI);

    /* ── Parse the body once into a JSONB blob. Every read below binds the
     * blob (no re-parse), and the same blob is archived. Keep this statement
     * open: the blob lives in its result memory until it is finalized. ── */
    sqlite3_stmt *j;
    if (sqlite3_prepare_v2(db, "SELECT jsonb(?1)", -1, &j, NULL) != SQLITE_OK) {
        /* Our-side failure (e.g. SQLITE_BUSY), not a bad response — archive the
         * raw body so a valid reply lost to DB contention is still recoverable. */
        archive_store(db, session_id, iteration_id, model, "ingest_error", body, -1, 0, request_body, NULL);
        return LLM_RESP_DBERR;
    }
    sqlite3_bind_text(j, 1, body, -1, SQLITE_STATIC);
    if (sqlite3_step(j) != SQLITE_ROW) {
        /* Not valid JSON at all — archive the raw text for forensics. */
        sqlite3_finalize(j);
        archive_store(db, session_id, iteration_id, model, "malformed", body, -1, 0, request_body, NULL);
        return LLM_RESP_MALFORMED;
    }
    const void *blob = sqlite3_column_blob(j, 0);
    int blen = sqlite3_column_bytes(j, 0);

    /* ── Scalar fields: bind the blob, hold the statement open so the
     * extracted column pointers stay valid through the entry inserts. ── */
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, gemini ? SCALAR_GEMINI : SCALAR_OPENAI, -1, &s, NULL) != SQLITE_OK) {
        /* Our-side failure — archive the (valid) body for forensics + recovery. */
        archive_store(db, session_id, iteration_id, model, "ingest_error", blob, blen, 1, request_body, NULL);
        sqlite3_finalize(j);
        return LLM_RESP_DBERR;
    }
    sqlite3_bind_blob(s, 1, blob, blen, SQLITE_STATIC);
    if (sqlite3_step(s) != SQLITE_ROW) {
        sqlite3_finalize(s);
        archive_store(db, session_id, iteration_id, model, "malformed", blob, blen, 1, request_body, NULL);
        sqlite3_finalize(j);
        return LLM_RESP_MALFORMED;
    }

    /* Shape check: choices/candidates array present and non-empty. */
    if (sqlite3_column_type(s, 7) == SQLITE_NULL || sqlite3_column_int(s, 7) == 0) {
        sqlite3_finalize(s);
        archive_store(db, session_id, iteration_id, model, "malformed", blob, blen, 1, request_body, NULL);
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
    RespUsage usage = { prompt_tokens, completion_tokens, -1, -1, -1, 0.0, 0 };
    if (sqlite3_column_type(s, 6) != SQLITE_NULL) {
        usage.cost = sqlite3_column_double(s, 6);
        usage.has_cost = 1;
        cost_nano = (int64_t)(usage.cost * 1e9 + 0.5);
    }
    if (sqlite3_column_type(s, 9) == SQLITE_INTEGER)  usage.cached_tokens = sqlite3_column_int(s, 9);
    if (sqlite3_column_type(s, 10) == SQLITE_INTEGER) usage.cache_write_tokens = sqlite3_column_int(s, 10);
    if (sqlite3_column_type(s, 11) == SQLITE_INTEGER) usage.reasoning_tokens = sqlite3_column_int(s, 11);

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
        archive_store(db, session_id, iteration_id, model, "empty", blob, blen, 1, request_body, NULL);
        sqlite3_finalize(j);
        return LLM_RESP_EMPTY;
    }

    /* Well-formed — archive before flattening into entries. */
    archive_store(db, session_id, iteration_id, model, "ok", blob, blen, 1, NULL, &usage);

    StopReason stop = map_stop_reason(finish);
    if (tc_count > 0 && stop != STOP_REASON_TOOL_USE)
        stop = STOP_REASON_TOOL_USE;

    int part = 0;

    /* Reasoning entry (scalar pointers still valid — s untouched). Two things
     * live here: the display text (kept only when save_reasoning, as before)
     * and the replay blob, which is captured unconditionally — the provider
     * requires it back verbatim on the next request, so dropping it because
     * an operator turned off reasoning *display* would break the wire (a
     * Gemini 400 / a silently invalidated prompt-cache prefix). The entry is
     * created whenever either half has something to say. */
    const char *replay_blob = (sqlite3_column_type(s, 8) != SQLITE_NULL)
                                  ? (const char *)sqlite3_column_text(s, 8) : NULL;
    const char *replay_text = (reasoning && reasoning[0]) ? reasoning : NULL;
    if (replay_blob || replay_text) {
        int64_t r_id = entry_append_typed(db, session_id, iteration_id, "reasoning", part++,
                                          save_reasoning ? replay_text : NULL,
                                          NULL, NULL, 0,
                                          STOP_REASON_NONE, model, 0, 0, 0);
        if (r_id > 0)
            reasoning_meta_set(db, r_id, gemini, model, replay_blob, replay_text);
    }

    int64_t asst_id = entry_append_typed(db, session_id, iteration_id, "assistant_message", part++,
                                         content, NULL, NULL, 0, stop, model,
                                         prompt_tokens, completion_tokens, cost_nano);
    sqlite3_finalize(s);   /* done with content/reasoning pointers */

    /* Cache-read count lands next to usage_in on the assistant entry — that is
     * the row rate_limit_check aggregates, and the archive row it also appears
     * on may be pruned or disabled entirely. Written only when the provider
     * reported a non-zero count, so a silent provider leaves the column NULL
     * and the limiter falls back to full weight. */
    if (asst_id > 0 && usage.cached_tokens > 0) {
        sqlite3_stmt *cu;
        if (sqlite3_prepare_v2(db, "UPDATE entries SET cached_tokens=?1 WHERE id=?2",
                               -1, &cu, NULL) == SQLITE_OK) {
            sqlite3_bind_int(cu, 1, usage.cached_tokens);
            sqlite3_bind_int64(cu, 2, asst_id);
            sqlite3_step(cu);
            sqlite3_finalize(cu);
        }
    }
    if (asst_id < 0) {
        if (tc) sqlite3_finalize(tc);
        sqlite3_finalize(j);
        return LLM_RESP_DBERR;
    }

    /* ── Tool call entries + tool_calls workflow rows ──
     * Arguments are validated with json() and stored in entries.content (single
     * source of truth). tool_calls holds only workflow state — no arguments.
     * Invalid JSON → immediate error tool_result so the model sees feedback. */
    if (tc && tc_count > 0) {
        const char *tc_ins_sql =
            "INSERT INTO tool_calls(session_id, entry_id, call_id, name)"
            " VALUES(?1,?2,?3,?4);";
        sqlite3_stmt *ins = NULL;
        sqlite3_prepare_v2(db, tc_ins_sql, -1, &ins, NULL);

        /* Validator: json() normalizes valid JSON, returns NULL on invalid. */
        sqlite3_stmt *jval = NULL;
        sqlite3_prepare_v2(db, "SELECT json(?1)", -1, &jval, NULL);

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

            /* Validate + normalize arguments JSON */
            const char *normalized = NULL;
            if (jval) {
                sqlite3_bind_text(jval, 1, args_val, -1, SQLITE_STATIC);
                if (sqlite3_step(jval) == SQLITE_ROW)
                    normalized = (const char *)sqlite3_column_text(jval, 0);
            }

            if (!normalized) {
                /* Invalid JSON from model — emit error tool_result directly */
                entry_append_typed(db, session_id, iteration_id, "tool_call", part++,
                                   "{}", id, name,
                                   0, STOP_REASON_NONE, NULL, 0, 0, 0);
                entry_append_typed(db, session_id, iteration_id, "tool_result", part++,
                                   "error: invalid JSON in tool call arguments",
                                   id, name, 1, STOP_REASON_NONE, NULL, 0, 0, 0);
                if (jval) sqlite3_reset(jval);
                idx++;
                continue;
            }

            int64_t tc_entry = entry_append_typed(db, session_id, iteration_id, "tool_call", part++,
                                                  normalized, id, name,
                                                  0, STOP_REASON_NONE, NULL, 0, 0, 0);
            if (jval) sqlite3_reset(jval);

            if (tc_entry > 0 && ins) {
                sqlite3_bind_int64(ins, 1, session_id);
                sqlite3_bind_int64(ins, 2, tc_entry);
                sqlite3_bind_text(ins, 3, id, -1, SQLITE_STATIC);
                sqlite3_bind_text(ins, 4, name ? name : "", -1, SQLITE_STATIC);
                sqlite3_step(ins);
                sqlite3_reset(ins);
            }
            idx++;
        }
        if (jval) sqlite3_finalize(jval);
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
