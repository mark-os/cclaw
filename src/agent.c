#define _POSIX_C_SOURCE 200809L
#include "agent.h"
#include "context.h"
#include "http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Build URL for chat completions endpoint */
static char *build_url(Arena *a, const Config *cfg) {
    const char *base = cfg->provider.base_url;
    size_t blen = strlen(base);
    /* Strip trailing slash */
    if (blen > 0 && base[blen - 1] == '/') blen--;
    const char *path = "/chat/completions";
    size_t plen = strlen(path);
    char *url = arena_alloc(a, blen + plen + 1);
    if (!url) return NULL;
    memcpy(url, base, blen);
    memcpy(url + blen, path, plen + 1);
    return url;
}

/* V10: dispatch a single tool call, always returns a result string */
static char *dispatch_tool(AgentContext *ctx, const ToolCall *tc) {
    if (!ctx->dispatch) {
        char *err = malloc(64);
        if (err) snprintf(err, 64, "error: no tool dispatcher registered");
        return err;
    }
    char *result = ctx->dispatch(tc->name, tc->arguments, ctx->dispatch_data);
    if (!result) {
        /* V10: never return NULL — produce error result */
        result = malloc(64);
        if (result) snprintf(result, 64, "error: tool '%s' returned null", tc->name);
    }
    return result;
}

int agent_run(AgentContext *ctx) {
    if (!ctx || !ctx->db || !ctx->cfg) return -1;

    int max_iter = ctx->cfg->max_iterations > 0 ? ctx->cfg->max_iterations : AGENT_DEFAULT_MAX_ITERATIONS;
    for (int iter = 0; iter < max_iter; iter++) {
        Arena *a = arena_create(ARENA_DEFAULT_SIZE);
        if (!a) return -1;

        /* Load branch and build context */
        int entry_count = 0;
        Entry *entries = session_get_branch(ctx->db, ctx->session_id, &entry_count);
        if (!entries && entry_count < 0) {
            arena_destroy(a);
            return -1;
        }

        Message *msgs = NULL;
        int msg_count = 0;
        int rc = context_build(entries, entry_count, ctx->cfg, &msgs, &msg_count);
        entry_branch_free(entries, entry_count);
        if (rc != 0 || msg_count == 0) {
            arena_destroy(a);
            return -1;
        }

        /* Build LLM request */
        char *req_json = llm_build_request(a, ctx->cfg, msgs, (size_t)msg_count,
                                           ctx->tools, ctx->tool_count);
        context_free(msgs, msg_count);
        if (!req_json) {
            arena_destroy(a);
            return -1;
        }

        /* Call LLM */
        char *url = build_url(a, ctx->cfg);
        if (!url) { arena_destroy(a); return -1; }

        if (ctx->debug)
            fprintf(stderr, "[DEBUG REQ] %s\n", req_json);

        char auth_hdr[512];
        snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s",
                 ctx->cfg->provider.api_key);
        const char *headers[] = {
            "Content-Type: application/json",
            auth_hdr,
            NULL
        };

        HttpResponse resp;
        int status = http_post(url, headers, req_json, &resp);
        if (status < 200 || status >= 300 || !resp.data) {
            if (ctx->debug && resp.data)
                fprintf(stderr, "[DEBUG RESP] status=%d %s\n", status, resp.data);
            http_response_free(&resp);
            arena_destroy(a);
            return -1;
        }

        if (ctx->debug)
            fprintf(stderr, "[DEBUG RESP] %s\n", resp.data);

        /* Parse response */
        LlmResponse llm_resp;
        rc = llm_parse_response(a, resp.data, &llm_resp);
        http_response_free(&resp);
        if (rc != 0) {
            arena_destroy(a);
            return -1;
        }

        /* If no tool calls — final response */
        if (llm_resp.tool_call_count == 0) {
            /* Append assistant message to session */
            Message asst = {.role = ROLE_ASSISTANT, .content = llm_resp.content ? strdup(llm_resp.content) : strdup("")};
            entry_append(ctx->db, ctx->session_id, &asst);
            free(asst.content);
            arena_destroy(a);
            return 0;
        }

        /* Has tool calls — append assistant message with tool_calls */
        Message asst = {0};
        asst.role = ROLE_ASSISTANT;
        asst.content = llm_resp.content ? strdup(llm_resp.content) : NULL;
        asst.tool_call_count = llm_resp.tool_call_count;
        asst.tool_calls = malloc(asst.tool_call_count * sizeof(ToolCall));
        if (!asst.tool_calls) {
            free(asst.content);
            arena_destroy(a);
            return -1;
        }
        for (size_t i = 0; i < asst.tool_call_count; i++) {
            asst.tool_calls[i].id = strdup(llm_resp.tool_calls[i].id);
            asst.tool_calls[i].name = strdup(llm_resp.tool_calls[i].name);
            asst.tool_calls[i].arguments = strdup(llm_resp.tool_calls[i].arguments);
        }
        entry_append(ctx->db, ctx->session_id, &asst);

        /* Dispatch each tool call and append results (V10) */
        for (size_t i = 0; i < asst.tool_call_count; i++) {
            char *result = dispatch_tool(ctx, &asst.tool_calls[i]);
            ToolResult tr = {.tool_call_id = asst.tool_calls[i].id, .content = result};
            Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr};
            entry_append(ctx->db, ctx->session_id, &tool_msg);
            free(result);
        }

        /* Cleanup heap copies */
        for (size_t i = 0; i < asst.tool_call_count; i++) {
            free(asst.tool_calls[i].id);
            free(asst.tool_calls[i].name);
            free(asst.tool_calls[i].arguments);
        }
        free(asst.tool_calls);
        free(asst.content);
        arena_destroy(a);
        /* Loop continues — next iteration will see tool results */
    }

    /* Max iterations reached */
    return -1;
}
