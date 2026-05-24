#define _POSIX_C_SOURCE 200809L
#include "agent.h"
#include "context.h"
#include "http.h"
#include "shutdown.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* V2: max retries for 429/5xx */
#define MAX_RETRIES 5
#define INITIAL_BACKOFF_MS 1000

/* V32: max LLM error retries per turn (parse failure, missing finish_reason) */
#define MAX_LLM_RETRIES 3

/* Build URL for chat completions endpoint */
static char *build_url(Arena *a, const Config *cfg) {
    const char *base = cfg->provider.base_url;
    size_t blen = strlen(base);
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
        result = malloc(64);
        if (result) snprintf(result, 64, "error: tool '%s' returned null", tc->name);
    }
    return result;
}

/* V2: call LLM with retry on 429 and 5xx. Returns status code, -1 on curl error. */
static int llm_call_with_retry(const char *url, const char **headers,
                               const char *body, HttpResponse *resp, int debug) {
    int backoff_ms = INITIAL_BACKOFF_MS;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        int status = http_post(url, headers, body, resp);

        if (status == 429 || (status >= 500 && status < 600)) {
            if (debug)
                fprintf(stderr, "[DEBUG] HTTP %d, retry %d/%d\n", status, attempt + 1, MAX_RETRIES);

            /* V2: respect Retry-After header */
            int wait_sec = resp->retry_after > 0 ? resp->retry_after : (backoff_ms / 1000);
            if (wait_sec < 1) wait_sec = 1;
            sleep((unsigned)wait_sec);

            http_response_free(resp);
            backoff_ms *= 2;
            continue;
        }

        return status;
    }

    /* All retries exhausted */
    return -1;
}

/* T45: try primary provider, then fallback chain on 5xx/timeout (-1) */
static int llm_call_with_fallback(Arena *a, const Config *cfg, const Message *msgs,
                                  size_t msg_count, const ToolSchema *tools,
                                  size_t tool_count, HttpResponse *resp, int debug) {
    /* Try primary */
    char *url = build_url(a, cfg);
    if (!url) return -1;

    char *req_json = llm_build_request(a, cfg, msgs, msg_count, tools, tool_count);
    if (!req_json) return -1;

    if (debug) fprintf(stderr, "[DEBUG REQ] %s\n", req_json);

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", cfg->provider.api_key);
    const char *headers[] = { "Content-Type: application/json", auth_hdr, NULL };

    int status = llm_call_with_retry(url, headers, req_json, resp, debug);

    /* If primary succeeded, return immediately */
    if (status != -1)
        return status;

    /* T45: try fallback providers (primary exhausted retries) */
    for (size_t i = 0; i < cfg->fallback_count; i++) {
        if (debug)
            fprintf(stderr, "[DEBUG] primary failed, trying fallback %zu\n", i);

        const ProviderConfig *fb = &cfg->fallback_providers[i];
        if (!fb->base_url || !fb->api_key || !fb->model) continue;

        /* Build URL for fallback */
        size_t blen = strlen(fb->base_url);
        if (blen > 0 && fb->base_url[blen - 1] == '/') blen--;
        const char *path = "/chat/completions";
        size_t plen = strlen(path);
        char *fb_url = arena_alloc(a, blen + plen + 1);
        if (!fb_url) continue;
        memcpy(fb_url, fb->base_url, blen);
        memcpy(fb_url + blen, path, plen + 1);

        /* Build request with fallback model */
        Config fb_cfg = *cfg;
        fb_cfg.provider = *fb;
        char *fb_req = llm_build_request(a, &fb_cfg, msgs, msg_count, tools, tool_count);
        if (!fb_req) continue;

        if (debug) fprintf(stderr, "[DEBUG REQ fallback %zu] %s\n", i, fb_req);

        char fb_auth[512];
        snprintf(fb_auth, sizeof(fb_auth), "Authorization: Bearer %s", fb->api_key);
        const char *fb_headers[] = { "Content-Type: application/json", fb_auth, NULL };

        status = llm_call_with_retry(fb_url, fb_headers, fb_req, resp, debug);
        if (status != -1)
            return status;
    }

    return status;
}

/* Detect context overflow from error response body */
static int is_context_overflow(const char *body) {
    if (!body) return 0;
    /* Common patterns from OpenAI-compatible APIs */
    return (strstr(body, "context_length_exceeded") != NULL ||
            strstr(body, "maximum context length") != NULL ||
            strstr(body, "too many tokens") != NULL ||
            strstr(body, "context window") != NULL);
}

int agent_run(AgentContext *ctx) {
    if (!ctx || !ctx->db || !ctx->cfg) return -1;

    int max_iter = ctx->cfg->max_iterations > 0 ? ctx->cfg->max_iterations : AGENT_DEFAULT_MAX_ITERATIONS;
    for (int iter = 0; iter < max_iter; iter++) {
        /* V31: check for graceful shutdown signal */
        if (shutdown_requested()) {
            int64_t tid = db_next_turn_id(ctx->db, ctx->session_id);
            Message abort_msg = {.role = ROLE_ASSISTANT,
                                 .content = strdup("error: agent terminated by shutdown signal"),
                                 .stop_reason = STOP_REASON_ABORTED};
            entry_append_with_turn(ctx->db, ctx->session_id, &abort_msg, tid);
            free(abort_msg.content);
            return -1;
        }

        Arena *a = arena_create(ARENA_DEFAULT_SIZE);
        if (!a) return -1;

        /* V17: all entries in this iteration share a turn_id */
        int64_t turn_id = db_next_turn_id(ctx->db, ctx->session_id);

        /* V32: per-turn LLM error retry loop */
        int llm_ok = 0;
        LlmResponse llm_resp;
        memset(&llm_resp, 0, sizeof(llm_resp));

        for (int retry = 0; retry < MAX_LLM_RETRIES; retry++) {
            /* Load branch and build context (re-load each retry for clean V28 state) */
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

            /* T45: call LLM with fallback chain */
            HttpResponse resp = {0};
            int status = llm_call_with_fallback(a, ctx->cfg, msgs, (size_t)msg_count,
                                                ctx->tools, ctx->tool_count, &resp, ctx->debug);
            context_free(msgs, msg_count);

            if (ctx->debug && resp.data)
                fprintf(stderr, "[DEBUG RESP] status=%d %s\n", status, resp.data);

            /* Context overflow detection — return error so caller can trim */
            if (status == 400 && is_context_overflow(resp.data)) {
                if (ctx->debug)
                    fprintf(stderr, "[DEBUG] context overflow detected\n");
                http_response_free(&resp);
                arena_destroy(a);
                return -2;
            }

            /* V29/V32: HTTP failure → retry */
            if (status < 200 || status >= 300 || !resp.data) {
                if (ctx->debug)
                    fprintf(stderr, "[DEBUG] LLM error (status=%d), retry %d/%d\n",
                            status, retry + 1, MAX_LLM_RETRIES);
                http_response_free(&resp);
                continue;
            }

            /* V10/V29: JSON parse failure → retry */
            rc = llm_parse_response(a, resp.data, &llm_resp);
            http_response_free(&resp);
            if (rc != 0) {
                if (ctx->debug)
                    fprintf(stderr, "[DEBUG] JSON parse failure, retry %d/%d\n",
                            retry + 1, MAX_LLM_RETRIES);
                continue;
            }

            /* V29: missing finish_reason → treat as error, retry */
            if (!llm_resp.finish_reason) {
                if (ctx->debug)
                    fprintf(stderr, "[DEBUG] missing finish_reason, retry %d/%d\n",
                            retry + 1, MAX_LLM_RETRIES);
                continue;
            }

            llm_ok = 1;
            break;
        }

        /* V32: all retries exhausted → write final error entry + exit */
        if (!llm_ok) {
            Message err_msg = {.role = ROLE_ASSISTANT,
                               .content = strdup("error: LLM request failed after retries"),
                               .stop_reason = STOP_REASON_ERROR};
            entry_append_with_turn(ctx->db, ctx->session_id, &err_msg, turn_id);
            free(err_msg.content);
            arena_destroy(a);
            return -1;
        }

        /* If no tool calls — final response */
        if (llm_resp.tool_call_count == 0) {
            Message asst = {.role = ROLE_ASSISTANT, .content = llm_resp.content ? strdup(llm_resp.content) : strdup("")};
            entry_append_with_turn(ctx->db, ctx->session_id, &asst, turn_id);
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
        entry_append_with_turn(ctx->db, ctx->session_id, &asst, turn_id);

        /* Dispatch each tool call and append results (V10) */
        for (size_t i = 0; i < asst.tool_call_count; i++) {
            /* V31: abort remaining tools on shutdown */
            if (shutdown_requested()) {
                ToolResult tr = {.tool_call_id = asst.tool_calls[i].id,
                                 .content = "error: agent terminated by shutdown signal"};
                Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr};
                entry_append_with_turn(ctx->db, ctx->session_id, &tool_msg, turn_id);
                for (size_t j = i + 1; j < asst.tool_call_count; j++) {
                    ToolResult skip_tr = {.tool_call_id = asst.tool_calls[j].id,
                                          .content = "error: agent terminated by shutdown signal"};
                    Message skip_msg = {.role = ROLE_TOOL, .tool_result = &skip_tr};
                    entry_append_with_turn(ctx->db, ctx->session_id, &skip_msg, turn_id);
                }
                for (size_t j = 0; j < asst.tool_call_count; j++) {
                    free(asst.tool_calls[j].id);
                    free(asst.tool_calls[j].name);
                    free(asst.tool_calls[j].arguments);
                }
                free(asst.tool_calls);
                free(asst.content);
                arena_destroy(a);
                return -1;
            }
            char *result = dispatch_tool(ctx, &asst.tool_calls[i]);
            ToolResult tr = {.tool_call_id = asst.tool_calls[i].id, .content = result};
            Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr};
            entry_append_with_turn(ctx->db, ctx->session_id, &tool_msg, turn_id);
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
    }

    /* Max iterations reached */
    return -1;
}
