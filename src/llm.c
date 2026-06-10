#include "llm.h"
#include "http.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* V35: sole normalization point for provider finish_reason → StopReason */
StopReason map_stop_reason(const char *finish_reason) {
    if (!finish_reason)
        return STOP_REASON_STOP;

    if (strcmp(finish_reason, "stop") == 0 ||
        strcmp(finish_reason, "end") == 0 ||
        strcmp(finish_reason, "end_turn") == 0)
        return STOP_REASON_STOP;

    if (strcmp(finish_reason, "length") == 0 ||
        strcmp(finish_reason, "max_tokens") == 0)
        return STOP_REASON_LENGTH;

    if (strcmp(finish_reason, "tool_calls") == 0 ||
        strcmp(finish_reason, "function_call") == 0 ||
        strcmp(finish_reason, "tool_use") == 0)
        return STOP_REASON_TOOL_USE;

    if (strcmp(finish_reason, "content_filter") == 0 ||
        strcmp(finish_reason, "network_error") == 0)
        return STOP_REASON_ERROR;

    return STOP_REASON_ERROR;
}

static char *arena_strdup(Arena *a, const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = arena_alloc(a, len + 1);
    if (dst) memcpy(dst, src, len + 1);
    return dst;
}

int llm_parse_response(sqlite3 *db, Arena *a, const char *json, LlmResponse *out) {
    if (!json || !out || !db) return -1;
    memset(out, 0, sizeof(*out));

    /* Extract scalar fields */
    const char *scalar_sql =
        "SELECT"
        " json_extract(?1,'$.choices[0].message.content'),"
        " json_extract(?1,'$.choices[0].finish_reason'),"
        " json_extract(?1,'$.usage.prompt_tokens'),"
        " json_extract(?1,'$.usage.completion_tokens'),"
        " json_extract(?1,'$.usage.total_tokens'),"
        " json_extract(?1,'$.usage.prompt_tokens_details.cached_tokens'),"
        " json_extract(?1,'$.usage.prompt_tokens_details.cache_write_tokens'),"
        " json_extract(?1,'$.usage.prompt_cache_hit_tokens'),"
        " json_extract(?1,'$.usage.completion_tokens_details.reasoning_tokens'),"
        " COALESCE(json_extract(?1,'$.usage.cost'), json_extract(?1,'$.usage.total_cost')),"
        " COALESCE(json_extract(?1,'$.choices[0].message.reasoning'),"
        "          json_extract(?1,'$.choices[0].message.reasoning_content'),"
        "          json_extract(?1,'$.choices[0].message.reasoning_text')),"
        " json_extract(?1,'$.choices[0].logprobs'),"
        " json_array_length(?1,'$.choices')";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, scalar_sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return -1;
    }

    /* Validate choices array exists and is non-empty */
    if (sqlite3_column_type(stmt, 12) == SQLITE_NULL || sqlite3_column_int(stmt, 12) == 0) {
        sqlite3_finalize(stmt);
        return -1;
    }

    const char *v;
    v = (const char *)sqlite3_column_text(stmt, 0);
    if (v) out->content = arena_strdup(a, v);
    v = (const char *)sqlite3_column_text(stmt, 1);
    if (v) out->finish_reason = arena_strdup(a, v);

    if (sqlite3_column_type(stmt, 2) == SQLITE_INTEGER)
        out->usage.prompt_tokens = sqlite3_column_int(stmt, 2);
    if (sqlite3_column_type(stmt, 3) == SQLITE_INTEGER)
        out->usage.completion_tokens = sqlite3_column_int(stmt, 3);
    if (sqlite3_column_type(stmt, 4) == SQLITE_INTEGER)
        out->usage.total_tokens = sqlite3_column_int(stmt, 4);
    if (sqlite3_column_type(stmt, 5) == SQLITE_INTEGER)
        out->usage.cache_read_tokens = sqlite3_column_int(stmt, 5);
    if (sqlite3_column_type(stmt, 6) == SQLITE_INTEGER)
        out->usage.cache_write_tokens = sqlite3_column_int(stmt, 6);
    if (sqlite3_column_type(stmt, 7) == SQLITE_INTEGER)
        out->usage.cache_read_tokens = sqlite3_column_int(stmt, 7);
    if (sqlite3_column_type(stmt, 8) == SQLITE_INTEGER)
        out->usage.reasoning_tokens = sqlite3_column_int(stmt, 8);
    if (sqlite3_column_type(stmt, 9) != SQLITE_NULL)
        out->usage.cost_nano = (int64_t)(sqlite3_column_double(stmt, 9) * 1e9 + 0.5);
    v = (const char *)sqlite3_column_text(stmt, 10);
    if (v) out->reasoning = arena_strdup(a, v);
    v = (const char *)sqlite3_column_text(stmt, 11);
    if (v) out->logprobs_json = arena_strdup(a, v);

    sqlite3_finalize(stmt);

    /* Extract tool_calls array */
    const char *tc_sql =
        "SELECT json_extract(value,'$.id'),"
        " json_extract(value,'$.function.name'),"
        " json_extract(value,'$.function.arguments')"
        " FROM json_each(?1,'$.choices[0].message.tool_calls')";
    if (sqlite3_prepare_v2(db, tc_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);

        /* Count first */
        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) count++;
        sqlite3_reset(stmt);

        if (count > 0) {
            out->tool_calls = arena_alloc(a, (size_t)count * sizeof(ToolCall));
            out->tool_call_count = (size_t)count;
            int i = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW && i < count) {
                const char *id = (const char *)sqlite3_column_text(stmt, 0);
                const char *name = (const char *)sqlite3_column_text(stmt, 1);
                const char *args = (const char *)sqlite3_column_text(stmt, 2);
                out->tool_calls[i].id = arena_strdup(a, id ? id : "");
                out->tool_calls[i].name = arena_strdup(a, name ? name : "");
                out->tool_calls[i].arguments = arena_strdup(a, args ? args : "");
                i++;
            }
        }
        sqlite3_finalize(stmt);
    }

    return 0;
}

/* T290: Parse native Gemini generateContent response. */
int llm_parse_response_gemini(sqlite3 *db, Arena *a, const char *json, LlmResponse *out) {
    if (!json || !out || !db) return -1;
    memset(out, 0, sizeof(*out));

    /* Validate candidates array */
    const char *check_sql = "SELECT json_array_length(?1,'$.candidates')";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
    int has_candidates = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) == SQLITE_INTEGER)
        has_candidates = sqlite3_column_int(stmt, 0) > 0;
    sqlite3_finalize(stmt);
    if (!has_candidates) return -1;

    /* Extract finishReason and map to OpenAI-style */
    const char *fr_sql = "SELECT json_extract(?1,'$.candidates[0].finishReason')";
    if (sqlite3_prepare_v2(db, fr_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *gr = (const char *)sqlite3_column_text(stmt, 0);
            if (gr) {
                if (strcmp(gr, "STOP") == 0) out->finish_reason = arena_strdup(a, "stop");
                else if (strcmp(gr, "MAX_TOKENS") == 0) out->finish_reason = arena_strdup(a, "length");
                else if (strcmp(gr, "SAFETY") == 0 || strcmp(gr, "RECITATION") == 0)
                    out->finish_reason = arena_strdup(a, "content_filter");
                else out->finish_reason = arena_strdup(a, gr);
            }
        }
        sqlite3_finalize(stmt);
    }

    /* Extract text parts (concatenate) */
    const char *text_sql =
        "SELECT group_concat(json_extract(value,'$.text'), char(10))"
        " FROM json_each(?1,'$.candidates[0].content.parts')"
        " WHERE json_extract(value,'$.text') IS NOT NULL";
    if (sqlite3_prepare_v2(db, text_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(stmt, 0);
            if (v) out->content = arena_strdup(a, v);
        }
        sqlite3_finalize(stmt);
    }

    /* Extract functionCall parts */
    const char *fc_sql =
        "SELECT json_extract(value,'$.functionCall.name'),"
        " json_extract(value,'$.functionCall.args')"
        " FROM json_each(?1,'$.candidates[0].content.parts')"
        " WHERE json_extract(value,'$.functionCall') IS NOT NULL";
    if (sqlite3_prepare_v2(db, fc_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) count++;
        sqlite3_reset(stmt);

        if (count > 0) {
            out->tool_calls = arena_alloc(a, (size_t)count * sizeof(ToolCall));
            out->tool_call_count = (size_t)count;
            int ti = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW && ti < count) {
                const char *name = (const char *)sqlite3_column_text(stmt, 0);
                const char *args = (const char *)sqlite3_column_text(stmt, 1);
                out->tool_calls[ti].name = arena_strdup(a, name ? name : "");
                char id_buf[32];
                snprintf(id_buf, sizeof(id_buf), "call_gemini_%d", ti);
                out->tool_calls[ti].id = arena_strdup(a, id_buf);
                out->tool_calls[ti].arguments = arena_strdup(a, args ? args : "{}");
                ti++;
            }
            if (!out->finish_reason || strcmp(out->finish_reason, "stop") == 0)
                out->finish_reason = arena_strdup(a, "tool_calls");
        }
        sqlite3_finalize(stmt);
    }

    /* usageMetadata */
    const char *usage_sql =
        "SELECT json_extract(?1,'$.usageMetadata.promptTokenCount'),"
        " json_extract(?1,'$.usageMetadata.candidatesTokenCount'),"
        " json_extract(?1,'$.usageMetadata.totalTokenCount'),"
        " json_extract(?1,'$.usageMetadata.cachedContentTokenCount')";
    if (sqlite3_prepare_v2(db, usage_sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, json, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            if (sqlite3_column_type(stmt, 0) == SQLITE_INTEGER)
                out->usage.prompt_tokens = sqlite3_column_int(stmt, 0);
            if (sqlite3_column_type(stmt, 1) == SQLITE_INTEGER)
                out->usage.completion_tokens = sqlite3_column_int(stmt, 1);
            if (sqlite3_column_type(stmt, 2) == SQLITE_INTEGER)
                out->usage.total_tokens = sqlite3_column_int(stmt, 2);
            else
                out->usage.total_tokens = out->usage.prompt_tokens + out->usage.completion_tokens;
            if (sqlite3_column_type(stmt, 3) == SQLITE_INTEGER)
                out->usage.cache_read_tokens = sqlite3_column_int(stmt, 3);
        }
        sqlite3_finalize(stmt);
    }

    return 0;
}

/* Populate LlmResponse directly from accumulated SSE context (no JSON round-trip). */
int llm_response_from_sse(Arena *a, const struct SseCtx *ctx, LlmResponse *out) {
    if (!a || !ctx || !out) return -1;
    memset(out, 0, sizeof(*out));
    if (ctx->content) out->content = arena_strdup(a, ctx->content);
    if (ctx->reasoning) out->reasoning = arena_strdup(a, ctx->reasoning);
    if (ctx->finish_reason) out->finish_reason = arena_strdup(a, ctx->finish_reason);
    if (ctx->tc_count > 0) {
        out->tool_calls = arena_alloc(a, ctx->tc_count * sizeof(ToolCall));
        out->tool_call_count = ctx->tc_count;
        for (size_t i = 0; i < ctx->tc_count; i++) {
            out->tool_calls[i].id = arena_strdup(a, ctx->tc_ids[i] ? ctx->tc_ids[i] : "");
            out->tool_calls[i].name = arena_strdup(a, ctx->tc_names[i] ? ctx->tc_names[i] : "");
            out->tool_calls[i].arguments = arena_strdup(a, ctx->tc_args[i] ? ctx->tc_args[i] : "");
        }
    }
    out->usage.prompt_tokens = ctx->prompt_tokens;
    out->usage.completion_tokens = ctx->completion_tokens;
    out->usage.total_tokens = ctx->total_tokens;
    out->usage.cache_read_tokens = ctx->cache_read_tokens;
    out->usage.cache_write_tokens = ctx->cache_write_tokens;
    out->usage.cost_nano = ctx->cost_nano;
    return 0;
}
