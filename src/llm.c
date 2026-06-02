#include "llm.h"
#include "cJSON.h"
#include <stdlib.h>
#include <string.h>

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

static const char *role_str(Role r) {
    switch (r) {
        case ROLE_SYSTEM:    return "system";
        case ROLE_USER:      return "user";
        case ROLE_ASSISTANT: return "assistant";
        case ROLE_TOOL:      return "tool";
        case ROLE_COMPACTION: return "user"; /* V58: summary shown as user msg */
    }
    return "user";
}

static cJSON *build_message(const Message *m) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    cJSON_AddStringToObject(obj, "role", role_str(m->role));

    if (m->role == ROLE_TOOL && m->tool_result) {
        cJSON_AddStringToObject(obj, "tool_call_id", m->tool_result->tool_call_id);
        cJSON_AddStringToObject(obj, "content", m->tool_result->content ? m->tool_result->content : "");
    } else if (m->role == ROLE_ASSISTANT && m->tool_calls && m->tool_call_count > 0) {
        if (m->content)
            cJSON_AddStringToObject(obj, "content", m->content);
        else
            cJSON_AddNullToObject(obj, "content");

        cJSON *tc_arr = cJSON_AddArrayToObject(obj, "tool_calls");
        for (size_t i = 0; i < m->tool_call_count; i++) {
            cJSON *tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", m->tool_calls[i].id);
            cJSON_AddStringToObject(tc, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", m->tool_calls[i].name);
            cJSON_AddStringToObject(fn, "arguments", m->tool_calls[i].arguments);
            cJSON_AddItemToObject(tc, "function", fn);
            cJSON_AddItemToArray(tc_arr, tc);
        }
    } else {
        cJSON_AddStringToObject(obj, "content", m->content ? m->content : "");
    }

    return obj;
}

char *llm_build_request(Arena *a, const Config *cfg, const Message *msgs,
                        size_t msg_count, const ToolSchema *tools,
                        size_t tool_count) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;

    cJSON_AddStringToObject(root, "model", cfg->provider.model);

    /* messages array */
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    for (size_t i = 0; i < msg_count; i++) {
        cJSON *m = build_message(&msgs[i]);
        if (!m) { cJSON_Delete(root); return NULL; }
        cJSON_AddItemToArray(messages, m);
    }

    /* V9: only include tools when tool_count > 0 */
    if (tools && tool_count > 0) {
        cJSON *tools_arr = cJSON_AddArrayToObject(root, "tools");
        for (size_t i = 0; i < tool_count; i++) {
            cJSON *tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", tools[i].name);
            if (tools[i].description)
                cJSON_AddStringToObject(fn, "description", tools[i].description);
            if (tools[i].parameters_json) {
                cJSON *params = cJSON_Parse(tools[i].parameters_json);
                if (params)
                    cJSON_AddItemToObject(fn, "parameters", params);
            }
            cJSON_AddItemToObject(tool, "function", fn);
            cJSON_AddItemToArray(tools_arr, tool);
        }
    }

    if (cfg->provider.max_tokens > 0)
        cJSON_AddNumberToObject(root, "max_tokens", cfg->provider.max_tokens);

    if (cfg->save_logprobs) {
        cJSON_AddBoolToObject(root, "logprobs", 1);
        cJSON_AddNumberToObject(root, "top_logprobs", 3);
    }

    /* Print to arena */
    char *printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) return NULL;

    size_t len = strlen(printed);
    char *result = arena_alloc(a, len + 1);
    if (!result) { free(printed); return NULL; }
    memcpy(result, printed, len + 1);
    free(printed);

    return result;
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
