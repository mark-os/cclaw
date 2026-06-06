#include "llm.h"
#include "http.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* V35: sole normalization point for provider finish_reason → StopReason */
StopReason map_stop_reason(const char *finish_reason) {
    if (!finish_reason)
        return STOP_REASON_STOP;  /* null treated as normal completion */

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

    /* unknown finish_reason → error */
    return STOP_REASON_ERROR;
}

/* Arena-duplicate a string. Returns NULL if src is NULL. */
static char *arena_strdup(Arena *a, const char *src) {
    if (!src) return NULL;
    size_t len = strlen(src);
    char *dst = arena_alloc(a, len + 1);
    if (dst) memcpy(dst, src, len + 1);
    return dst;
}

int llm_parse_response(Arena *a, const char *json, LlmResponse *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    /* Extract choices[0].message */
    cJSON *choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || cJSON_GetArraySize(choices) == 0) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
    cJSON *message = cJSON_GetObjectItem(choice0, "message");
    if (!message) { cJSON_Delete(root); return -1; }

    /* finish_reason */
    cJSON *fr = cJSON_GetObjectItem(choice0, "finish_reason");
    if (fr && cJSON_IsString(fr))
        out->finish_reason = arena_strdup(a, fr->valuestring);

    /* content */
    cJSON *content = cJSON_GetObjectItem(message, "content");
    if (content && cJSON_IsString(content))
        out->content = arena_strdup(a, content->valuestring);

    /* tool_calls */
    cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
    if (tool_calls && cJSON_IsArray(tool_calls)) {
        int count = cJSON_GetArraySize(tool_calls);
        if (count > 0) {
            out->tool_calls = arena_alloc(a, (size_t)count * sizeof(ToolCall));
            if (!out->tool_calls) { cJSON_Delete(root); return -1; }
            out->tool_call_count = (size_t)count;

            for (int i = 0; i < count; i++) {
                cJSON *tc = cJSON_GetArrayItem(tool_calls, i);
                cJSON *id = cJSON_GetObjectItem(tc, "id");
                cJSON *fn = cJSON_GetObjectItem(tc, "function");

                out->tool_calls[i].id = arena_strdup(a, id && cJSON_IsString(id) ? id->valuestring : "");
                if (fn) {
                    cJSON *name = cJSON_GetObjectItem(fn, "name");
                    cJSON *args = cJSON_GetObjectItem(fn, "arguments");
                    out->tool_calls[i].name = arena_strdup(a, name && cJSON_IsString(name) ? name->valuestring : "");
                    out->tool_calls[i].arguments = arena_strdup(a, args && cJSON_IsString(args) ? args->valuestring : "");
                } else {
                    out->tool_calls[i].name = arena_strdup(a, "");
                    out->tool_calls[i].arguments = arena_strdup(a, "");
                }
            }
        }
    }

    /* usage */
    cJSON *usage = cJSON_GetObjectItem(root, "usage");
    if (usage) {
        cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
        cJSON *tt = cJSON_GetObjectItem(usage, "total_tokens");
        if (pt && cJSON_IsNumber(pt)) out->usage.prompt_tokens = pt->valueint;
        if (ct && cJSON_IsNumber(ct)) out->usage.completion_tokens = ct->valueint;
        if (tt && cJSON_IsNumber(tt)) out->usage.total_tokens = tt->valueint;

        /* Cache tokens — multiple field locations across providers */
        cJSON *ptd = cJSON_GetObjectItem(usage, "prompt_tokens_details");
        if (ptd) {
            cJSON *cr = cJSON_GetObjectItem(ptd, "cached_tokens");
            cJSON *cw = cJSON_GetObjectItem(ptd, "cache_write_tokens");
            if (cr && cJSON_IsNumber(cr)) out->usage.cache_read_tokens = cr->valueint;
            if (cw && cJSON_IsNumber(cw)) out->usage.cache_write_tokens = cw->valueint;
        }
        /* DeepSeek direct uses top-level field */
        cJSON *pch = cJSON_GetObjectItem(usage, "prompt_cache_hit_tokens");
        if (pch && cJSON_IsNumber(pch)) out->usage.cache_read_tokens = pch->valueint;

        /* Reasoning tokens (subset of completion) */
        cJSON *ctd = cJSON_GetObjectItem(usage, "completion_tokens_details");
        if (ctd) {
            cJSON *rt = cJSON_GetObjectItem(ctd, "reasoning_tokens");
            if (rt && cJSON_IsNumber(rt)) out->usage.reasoning_tokens = rt->valueint;
        }

        /* Cost — OpenRouter reports as "cost" (dollars) in usage */
        cJSON *cost_field = cJSON_GetObjectItem(usage, "cost");
        if (!cost_field || !cJSON_IsNumber(cost_field))
            cost_field = cJSON_GetObjectItem(usage, "total_cost");
        if (cost_field && cJSON_IsNumber(cost_field))
            out->usage.cost_nano = (int64_t)(cost_field->valuedouble * 1e9 + 0.5);
    }

    /* reasoning/thinking — try all known field names */
    cJSON *reasoning = cJSON_GetObjectItem(message, "reasoning");
    if (!reasoning || !cJSON_IsString(reasoning))
        reasoning = cJSON_GetObjectItem(message, "reasoning_content");
    if (!reasoning || !cJSON_IsString(reasoning))
        reasoning = cJSON_GetObjectItem(message, "reasoning_text");
    if (reasoning && cJSON_IsString(reasoning))
        out->reasoning = arena_strdup(a, reasoning->valuestring);

    /* logprobs */
    cJSON *logprobs = cJSON_GetObjectItem(choice0, "logprobs");
    if (logprobs && !cJSON_IsNull(logprobs)) {
        char *lp_str = cJSON_PrintUnformatted(logprobs);
        if (lp_str) {
            out->logprobs_json = arena_strdup(a, lp_str);
            free(lp_str);
        }
    }

    cJSON_Delete(root);
    return 0;
}

/* T290: Parse native Gemini generateContent response.
 * Format: candidates[0].content.parts[] with text/functionCall parts,
 * finishReason at candidate level, usageMetadata at root. */
int llm_parse_response_gemini(Arena *a, const char *json, LlmResponse *out) {
    if (!json || !out) return -1;
    memset(out, 0, sizeof(*out));

    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    cJSON *candidates = cJSON_GetObjectItem(root, "candidates");
    if (!candidates || cJSON_GetArraySize(candidates) == 0) {
        cJSON_Delete(root);
        return -1;
    }

    cJSON *cand0 = cJSON_GetArrayItem(candidates, 0);
    cJSON *content = cJSON_GetObjectItem(cand0, "content");
    cJSON *parts = content ? cJSON_GetObjectItem(content, "parts") : NULL;

    /* finishReason — Gemini uses UPPER_CASE: STOP, MAX_TOKENS, SAFETY, etc. */
    cJSON *fr = cJSON_GetObjectItem(cand0, "finishReason");
    if (fr && cJSON_IsString(fr)) {
        /* Map Gemini finishReason to OpenAI-style for map_stop_reason */
        const char *greason = fr->valuestring;
        if (strcmp(greason, "STOP") == 0)
            out->finish_reason = arena_strdup(a, "stop");
        else if (strcmp(greason, "MAX_TOKENS") == 0)
            out->finish_reason = arena_strdup(a, "length");
        else if (strcmp(greason, "SAFETY") == 0 || strcmp(greason, "RECITATION") == 0)
            out->finish_reason = arena_strdup(a, "content_filter");
        else
            out->finish_reason = arena_strdup(a, greason);
    }

    if (parts && cJSON_IsArray(parts)) {
        int nparts = cJSON_GetArraySize(parts);

        /* Count text and functionCall parts */
        size_t text_total = 0;
        int fc_count = 0;
        for (int i = 0; i < nparts; i++) {
            cJSON *part = cJSON_GetArrayItem(parts, i);
            cJSON *text = cJSON_GetObjectItem(part, "text");
            if (text && cJSON_IsString(text))
                text_total += strlen(text->valuestring) + 1;
            if (cJSON_GetObjectItem(part, "functionCall"))
                fc_count++;
        }

        /* Concatenate text parts */
        if (text_total > 0) {
            char *buf = arena_alloc(a, text_total);
            if (buf) {
                buf[0] = '\0';
                for (int i = 0; i < nparts; i++) {
                    cJSON *part = cJSON_GetArrayItem(parts, i);
                    cJSON *text = cJSON_GetObjectItem(part, "text");
                    if (text && cJSON_IsString(text)) {
                        if (buf[0]) strcat(buf, "\n");
                        strcat(buf, text->valuestring);
                    }
                }
                out->content = buf;
            }
        }

        /* Extract functionCall parts → tool_calls */
        if (fc_count > 0) {
            out->tool_calls = arena_alloc(a, (size_t)fc_count * sizeof(ToolCall));
            if (out->tool_calls) {
                out->tool_call_count = (size_t)fc_count;
                int ti = 0;
                for (int i = 0; i < nparts && ti < fc_count; i++) {
                    cJSON *part = cJSON_GetArrayItem(parts, i);
                    cJSON *fc = cJSON_GetObjectItem(part, "functionCall");
                    if (!fc) continue;
                    cJSON *name = cJSON_GetObjectItem(fc, "name");
                    cJSON *args = cJSON_GetObjectItem(fc, "args");

                    out->tool_calls[ti].name = arena_strdup(a, name && cJSON_IsString(name) ? name->valuestring : "");
                    /* Generate a synthetic ID (Gemini doesn't provide one) */
                    char id_buf[32];
                    snprintf(id_buf, sizeof(id_buf), "call_gemini_%d", ti);
                    out->tool_calls[ti].id = arena_strdup(a, id_buf);
                    /* args is an object — stringify it */
                    if (args) {
                        char *args_str = cJSON_PrintUnformatted(args);
                        out->tool_calls[ti].arguments = arena_strdup(a, args_str ? args_str : "{}");
                        free(args_str);
                    } else {
                        out->tool_calls[ti].arguments = arena_strdup(a, "{}");
                    }
                    ti++;
                }
                /* Set finish_reason to tool_calls if we have function calls */
                if (!out->finish_reason || strcmp(out->finish_reason, "stop") == 0)
                    out->finish_reason = arena_strdup(a, "tool_calls");
            }
        }
    }

    /* usageMetadata */
    cJSON *usage = cJSON_GetObjectItem(root, "usageMetadata");
    if (usage) {
        cJSON *pt = cJSON_GetObjectItem(usage, "promptTokenCount");
        cJSON *ct = cJSON_GetObjectItem(usage, "candidatesTokenCount");
        cJSON *tt = cJSON_GetObjectItem(usage, "totalTokenCount");
        if (pt && cJSON_IsNumber(pt)) out->usage.prompt_tokens = pt->valueint;
        if (ct && cJSON_IsNumber(ct)) out->usage.completion_tokens = ct->valueint;
        if (tt && cJSON_IsNumber(tt)) out->usage.total_tokens = tt->valueint;
        else out->usage.total_tokens = out->usage.prompt_tokens + out->usage.completion_tokens;
        /* Cached token support */
        cJSON *cached = cJSON_GetObjectItem(usage, "cachedContentTokenCount");
        if (cached && cJSON_IsNumber(cached)) out->usage.cache_read_tokens = cached->valueint;
    }

    cJSON_Delete(root);
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
