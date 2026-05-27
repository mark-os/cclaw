#define _POSIX_C_SOURCE 200809L
#include "agent.h"
#include "context.h"
#include "request_stream.h"
#include "http.h"
#include "shutdown.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* V45: plan-only re-prompt */
#define PLAN_RETRY_PROMPT "Do not restate the plan. Act now: take the first concrete tool action. If blocked, state the blocker in one sentence."

/* V2: max retries for 429/5xx */
#define MAX_RETRIES 5
#define INITIAL_BACKOFF_MS 1000

/* V43: tool loop detection */
#define TOOL_LOOP_RING_SIZE 30
#define TOOL_LOOP_WARN_THRESHOLD 5
#define TOOL_LOOP_BREAK_THRESHOLD 10

/* V32: max LLM error retries per turn (parse failure, missing finish_reason) */
#define MAX_LLM_RETRIES 3

/* V43: tool loop ring buffer */
typedef struct {
    uint32_t hashes[TOOL_LOOP_RING_SIZE];
    int count;  /* total calls recorded (may exceed RING_SIZE) */
} ToolLoopRing;

static uint32_t tool_call_hash(const char *name, const char *args) {
    /* FNV-1a 32-bit */
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 16777619u;
    }
    if (args) {
        for (const char *p = args; *p; p++) {
            h ^= (uint8_t)*p;
            h *= 16777619u;
        }
    }
    return h;
}

/* Count consecutive occurrences of hash at tail of ring buffer */
static int tool_loop_streak(const ToolLoopRing *ring, uint32_t hash) {
    int n = ring->count < TOOL_LOOP_RING_SIZE ? ring->count : TOOL_LOOP_RING_SIZE;
    int tail = (ring->count - 1) % TOOL_LOOP_RING_SIZE;
    int streak = 0;
    for (int i = 0; i < n; i++) {
        int idx = tail - i;
        if (idx < 0) idx += TOOL_LOOP_RING_SIZE;
        if (ring->hashes[idx] == hash) streak++;
        else break;
    }
    return streak;
}

/* V45: detect plan-only response (bullets + promise verbs, no concrete action) */
static int is_plan_only(const char *content) {
    if (!content || !*content) return 0;
    /* Must have bullet points or numbered list */
    int has_bullets = (strstr(content, "\n- ") || strstr(content, "\n* ") ||
                       strstr(content, "\n1.") || strstr(content, "\n1)"));
    if (!has_bullets) return 0;
    /* Must have promise verbs indicating intent without action */
    static const char *promises[] = {
        "I'll ", "I will ", "Let me ", "I'm going to ",
        "Here's my plan", "Here is my plan", "I can ",
        "I'll:", "I will:", "Let me:", NULL
    };
    for (int i = 0; promises[i]; i++) {
        if (strstr(content, promises[i])) return 1;
    }
    return 0;
}

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
    /* T115: notify progress — tool starting */
    if (ctx->progress)
        ctx->progress(PROGRESS_TOOL_START, tc->name, tc->arguments, ctx->progress_data);

    char *result = ctx->dispatch(tc->name, tc->arguments, ctx->dispatch_data);
    if (!result) {
        result = malloc(64);
        if (result) snprintf(result, 64, "error: tool '%s' returned null", tc->name);
    }
    return result;
}

/* V41,V2: call LLM with streaming upload + retry on 429/5xx. Resets streamer on retry. */
static int llm_call_with_retry_stream(const char *url, const char **headers,
                                      RequestStreamer *rs, HttpResponse *resp, int debug) {
    int backoff_ms = INITIAL_BACKOFF_MS;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        int status = http_post_stream(url, headers, rs_read_cb, rs, resp);

        if (status == 429 || (status >= 500 && status < 600)) {
            if (debug)
                fprintf(stderr, "[DEBUG] HTTP %d, retry %d/%d\n", status, attempt + 1, MAX_RETRIES);

            int wait_sec = resp->retry_after > 0 ? resp->retry_after : (backoff_ms / 1000);
            if (wait_sec < 1) wait_sec = 1;
            sleep((unsigned)wait_sec);

            http_response_free(resp);
            rs_reset(rs);
            backoff_ms *= 2;
            continue;
        }

        return status;
    }

    return -1;
}

/* T45,V41: try primary provider with streaming, then fallback chain on 5xx/timeout (-1) */
static int llm_call_with_fallback_stream(Arena *a, const Config *cfg,
                                         sqlite3 *db, int64_t session_id,
                                         const ContextPlan *plan,
                                         const ToolSchema *tools, size_t tool_count,
                                         HttpResponse *resp, int debug) {
    /* Mock mode: read canned response from file, skip HTTP entirely */
    const char *mock_path = getenv("CCLAW_LLM_MOCK");
    if (mock_path) {
        FILE *f = fopen(mock_path, "r");
        if (!f) return -1;
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);
        resp->data = malloc((size_t)len + 1);
        if (resp->data) {
            fread(resp->data, 1, (size_t)len, f);
            resp->data[len] = '\0';
            resp->len = (size_t)len;
        }
        fclose(f);
        (void)a; (void)cfg; (void)db; (void)session_id;
        (void)plan; (void)tools; (void)tool_count; (void)debug;
        return resp->data ? 200 : -1;
    }

    char *url = build_url(a, cfg);
    if (!url) return -1;

    RequestStreamer rs;
    if (rs_init(&rs, db, session_id, cfg, plan, tools, tool_count) != 0)
        return -1;

    if (debug) {
        /* For debug: stream entire request to stderr */
        RequestStreamer dbg_rs;
        rs_init(&dbg_rs, db, session_id, cfg, plan, tools, tool_count);
        size_t cap = 4096, len = 0;
        char *dbuf = malloc(cap);
        if (dbuf) {
            while (1) {
                if (len + 1024 > cap) { cap *= 2; dbuf = realloc(dbuf, cap); if (!dbuf) break; }
                size_t n = rs_read_cb(dbuf + len, 1, 1024, &dbg_rs);
                if (n == 0) break;
                len += n;
            }
            if (dbuf) { dbuf[len] = '\0'; fprintf(stderr, "[DEBUG REQ] %s\n", dbuf); free(dbuf); }
        }
        rs_cleanup(&dbg_rs);
    }

    char auth_hdr[512];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", cfg->provider.api_key);
    const char *headers[] = { "Content-Type: application/json", auth_hdr, NULL };

    int status = llm_call_with_retry_stream(url, headers, &rs, resp, debug);
    rs_cleanup(&rs);

    if (status != -1)
        return status;

    /* T45: try fallback providers */
    for (size_t i = 0; i < cfg->fallback_count; i++) {
        if (debug)
            fprintf(stderr, "[DEBUG] primary failed, trying fallback %zu\n", i);

        const ProviderConfig *fb = &cfg->fallback_providers[i];
        if (!fb->base_url || !fb->api_key || !fb->model) continue;

        size_t blen = strlen(fb->base_url);
        if (blen > 0 && fb->base_url[blen - 1] == '/') blen--;
        const char *path = "/chat/completions";
        size_t plen = strlen(path);
        char *fb_url = arena_alloc(a, blen + plen + 1);
        if (!fb_url) continue;
        memcpy(fb_url, fb->base_url, blen);
        memcpy(fb_url + blen, path, plen + 1);

        /* Build streamer with fallback config */
        Config fb_cfg = *cfg;
        fb_cfg.provider = *fb;

        RequestStreamer fb_rs;
        if (rs_init(&fb_rs, db, session_id, &fb_cfg, plan, tools, tool_count) != 0)
            continue;

        if (debug) {
            RequestStreamer dbg_rs;
            rs_init(&dbg_rs, db, session_id, &fb_cfg, plan, tools, tool_count);
            size_t cap = 4096, len = 0;
            char *dbuf = malloc(cap);
            if (dbuf) {
                while (1) {
                    if (len + 1024 > cap) { cap *= 2; dbuf = realloc(dbuf, cap); if (!dbuf) break; }
                    size_t n = rs_read_cb(dbuf + len, 1, 1024, &dbg_rs);
                    if (n == 0) break;
                    len += n;
                }
                if (dbuf) { dbuf[len] = '\0'; fprintf(stderr, "[DEBUG REQ fallback %zu] %s\n", i, dbuf); free(dbuf); }
            }
            rs_cleanup(&dbg_rs);
        }

        char fb_auth[512];
        snprintf(fb_auth, sizeof(fb_auth), "Authorization: Bearer %s", fb->api_key);
        const char *fb_headers[] = { "Content-Type: application/json", fb_auth, NULL };

        status = llm_call_with_retry_stream(fb_url, fb_headers, &fb_rs, resp, debug);
        rs_cleanup(&fb_rs);
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

/* Build metadata JSON from LLM response based on config flags. Returns heap string or NULL. */
static char *build_metadata(const Config *cfg, const LlmResponse *resp) {
    if (!cfg || (!cfg->save_reasoning && !cfg->save_usage && !cfg->save_logprobs))
        return NULL;

    cJSON *meta = cJSON_CreateObject();

    if (cfg->save_reasoning && resp->reasoning)
        cJSON_AddStringToObject(meta, "reasoning", resp->reasoning);

    if (cfg->save_usage && resp->usage.total_tokens > 0) {
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "prompt_tokens", resp->usage.prompt_tokens);
        cJSON_AddNumberToObject(u, "completion_tokens", resp->usage.completion_tokens);
        cJSON_AddNumberToObject(u, "total_tokens", resp->usage.total_tokens);
        if (resp->usage.cache_read_tokens)
            cJSON_AddNumberToObject(u, "cache_read_tokens", resp->usage.cache_read_tokens);
        if (resp->usage.cache_write_tokens)
            cJSON_AddNumberToObject(u, "cache_write_tokens", resp->usage.cache_write_tokens);
        if (resp->usage.reasoning_tokens)
            cJSON_AddNumberToObject(u, "reasoning_tokens", resp->usage.reasoning_tokens);
        cJSON_AddItemToObject(meta, "usage", u);
    }

    if (cfg->save_logprobs && resp->logprobs_json) {
        cJSON *lp = cJSON_Parse(resp->logprobs_json);
        if (lp) cJSON_AddItemToObject(meta, "logprobs", lp);
    }

    /* Don't store empty metadata */
    if (!meta->child) { cJSON_Delete(meta); return NULL; }

    char *json = cJSON_PrintUnformatted(meta);
    cJSON_Delete(meta);
    return json;
}

int agent_run(AgentContext *ctx) {
    if (!ctx || !ctx->db || !ctx->cfg) return -1;

    /* V43: tool loop detection ring buffer (persists across iterations) */
    ToolLoopRing loop_ring = {.count = 0};

    /* V45: plan-only retry (max 1) */
    int plan_retried = 0;

    /* Track whether prior assistant content exists in this turn (for empty response handling) */
    int has_prior_content = 0;

    int max_iter = ctx->cfg->max_iterations > 0 ? ctx->cfg->max_iterations : AGENT_DEFAULT_MAX_ITERATIONS;
    for (int iter = 0; iter < max_iter; iter++) {
        /* V31: check for graceful shutdown signal */
        if (shutdown_requested()) {
            int64_t tid = db_next_turn_id(ctx->db, ctx->session_id);
            Message abort_msg = {.role = ROLE_ASSISTANT,
                                 .content = strdup("error: agent terminated by shutdown signal"),
                                 .stop_reason = STOP_REASON_ABORTED,
                                 .model = ctx->cfg->provider.model};
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
            /* V41: plan entry IDs + cut point from SQLite (pass 1, no content loaded) */
            ContextPlan plan;
            int rc = context_plan(ctx->db, ctx->session_id, ctx->cfg, &plan);
            if (rc != 0) {
                arena_destroy(a);
                return -1;
            }

            /* T45,V41: call LLM with streaming request + fallback chain */
            HttpResponse resp = {0};
            int status = llm_call_with_fallback_stream(a, ctx->cfg, ctx->db,
                                                       ctx->session_id, &plan,
                                                       ctx->tools, ctx->tool_count,
                                                       &resp, ctx->debug);
            context_plan_free(&plan);

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
                               .stop_reason = STOP_REASON_ERROR,
                               .model = ctx->cfg->provider.model};
            entry_append_with_turn(ctx->db, ctx->session_id, &err_msg, turn_id);
            free(err_msg.content);
            arena_destroy(a);
            return -1;
        }

        /* If no tool calls — final response */
        if (llm_resp.tool_call_count == 0) {
            StopReason sr = map_stop_reason(llm_resp.finish_reason);

            /* V29: stop + null/empty content with no prior assistant content = error */
            if (sr == STOP_REASON_STOP &&
                (!llm_resp.content || llm_resp.content[0] == '\0') &&
                !has_prior_content) {
                char *meta = build_metadata(ctx->cfg, &llm_resp);
                Message err_msg = {.role = ROLE_ASSISTANT,
                                   .content = strdup("error: empty response from LLM"),
                                   .stop_reason = STOP_REASON_ERROR,
                                   .metadata_json = meta,
                                   .model = ctx->cfg->provider.model,
                                   .usage_in = llm_resp.usage.prompt_tokens,
                                   .usage_out = llm_resp.usage.completion_tokens};
                entry_append_with_turn(ctx->db, ctx->session_id, &err_msg, turn_id);
                free(err_msg.content);
                free(meta);
                arena_destroy(a);
                return -1;
            }

            char *meta = build_metadata(ctx->cfg, &llm_resp);
            Message asst = {.role = ROLE_ASSISTANT,
                            .content = llm_resp.content ? strdup(llm_resp.content) : strdup(""),
                            .stop_reason = sr,
                            .metadata_json = meta,
                            .model = ctx->cfg->provider.model,
                            .usage_in = llm_resp.usage.prompt_tokens,
                            .usage_out = llm_resp.usage.completion_tokens};
            entry_append_with_turn(ctx->db, ctx->session_id, &asst, turn_id);

            /* V45: plan-only retry — re-prompt once if response is just a plan */
            if (!plan_retried && sr == STOP_REASON_STOP && is_plan_only(asst.content)) {
                plan_retried = 1;
                Message reprompt = {.role = ROLE_USER,
                                    .content = (char *)PLAN_RETRY_PROMPT};
                entry_append_with_turn(ctx->db, ctx->session_id, &reprompt, turn_id);
                free(asst.content);
                free(meta);
                arena_destroy(a);
                continue;
            }

            free(asst.content);
            free(meta);
            arena_destroy(a);
            return 0;
        }

        /* Has tool calls — append assistant message with tool_calls */
        has_prior_content = 1;
        Message asst = {0};
        asst.role = ROLE_ASSISTANT;
        asst.content = llm_resp.content ? strdup(llm_resp.content) : NULL;
        asst.stop_reason = map_stop_reason(llm_resp.finish_reason);
        asst.tool_call_count = llm_resp.tool_call_count;
        asst.metadata_json = build_metadata(ctx->cfg, &llm_resp);
        asst.model = ctx->cfg->provider.model;
        asst.usage_in = llm_resp.usage.prompt_tokens;
        asst.usage_out = llm_resp.usage.completion_tokens;

        /* T115: notify progress — intermediate assistant text */
        if (ctx->progress && asst.content && asst.content[0])
            ctx->progress(PROGRESS_ASSISTANT_TEXT, NULL, asst.content, ctx->progress_data);
        asst.tool_calls = malloc(asst.tool_call_count * sizeof(ToolCall));
        if (!asst.tool_calls) {
            free(asst.content);
            free(asst.metadata_json);
            arena_destroy(a);
            return -1;
        }
        for (size_t i = 0; i < asst.tool_call_count; i++) {
            asst.tool_calls[i].id = strdup(llm_resp.tool_calls[i].id);
            asst.tool_calls[i].name = strdup(llm_resp.tool_calls[i].name);
            asst.tool_calls[i].arguments = strdup(llm_resp.tool_calls[i].arguments);
        }
        entry_append_with_turn(ctx->db, ctx->session_id, &asst, turn_id);

        /* Dispatch each tool call and append results (V10, V43) */
        int loop_break = 0;
        for (size_t i = 0; i < asst.tool_call_count; i++) {
            /* V31: abort remaining tools on shutdown */
            if (shutdown_requested()) {
                ToolResult tr = {.tool_call_id = asst.tool_calls[i].id,
                                 .content = "error: agent terminated by shutdown signal"};
                Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                    .tool_name = asst.tool_calls[i].name, .is_error = 1};
                entry_append_with_turn(ctx->db, ctx->session_id, &tool_msg, turn_id);
                for (size_t j = i + 1; j < asst.tool_call_count; j++) {
                    ToolResult skip_tr = {.tool_call_id = asst.tool_calls[j].id,
                                          .content = "error: agent terminated by shutdown signal"};
                    Message skip_msg = {.role = ROLE_TOOL, .tool_result = &skip_tr,
                                        .tool_name = asst.tool_calls[j].name, .is_error = 1};
                    entry_append_with_turn(ctx->db, ctx->session_id, &skip_msg, turn_id);
                }
                for (size_t j = 0; j < asst.tool_call_count; j++) {
                    free(asst.tool_calls[j].id);
                    free(asst.tool_calls[j].name);
                    free(asst.tool_calls[j].arguments);
                }
                free(asst.tool_calls);
                free(asst.content);
                free(asst.metadata_json);
                arena_destroy(a);
                return -1;
            }

            /* V43: tool loop detection */
            uint32_t h = tool_call_hash(asst.tool_calls[i].name, asst.tool_calls[i].arguments);
            int streak = tool_loop_streak(&loop_ring, h) + 1; /* +1 for this call */

            if (streak >= TOOL_LOOP_BREAK_THRESHOLD) {
                /* ≥10: force-stop agent loop */
                char *err = strdup("error: tool loop detected — same call repeated 10+ times with no progress");
                ToolResult tr = {.tool_call_id = asst.tool_calls[i].id, .content = err};
                Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                    .tool_name = asst.tool_calls[i].name, .is_error = 1};
                entry_append_with_turn(ctx->db, ctx->session_id, &tool_msg, turn_id);
                free(err);
                /* Write error results for remaining tool calls */
                for (size_t j = i + 1; j < asst.tool_call_count; j++) {
                    ToolResult skip_tr = {.tool_call_id = asst.tool_calls[j].id,
                                          .content = "error: tool loop detected"};
                    Message skip_msg = {.role = ROLE_TOOL, .tool_result = &skip_tr,
                                        .tool_name = asst.tool_calls[j].name, .is_error = 1};
                    entry_append_with_turn(ctx->db, ctx->session_id, &skip_msg, turn_id);
                }
                loop_ring.hashes[loop_ring.count % TOOL_LOOP_RING_SIZE] = h;
                loop_ring.count++;
                loop_break = 1;
                break;
            }

            char *result = dispatch_tool(ctx, &asst.tool_calls[i]);

            /* T115: notify progress — tool result (truncated for display) */
            if (ctx->progress)
                ctx->progress(PROGRESS_TOOL_RESULT, asst.tool_calls[i].name,
                              result, ctx->progress_data);

            /* V43: ≥5 same → inject warning into result */
            if (streak >= TOOL_LOOP_WARN_THRESHOLD) {
                const char *warn = "\n\n[WARNING: You have called this tool with identical arguments "
                                   "multiple times. Try a different approach or stop.]";
                size_t rlen = strlen(result);
                size_t wlen = strlen(warn);
                char *augmented = realloc(result, rlen + wlen + 1);
                if (augmented) {
                    memcpy(augmented + rlen, warn, wlen + 1);
                    result = augmented;
                }
            }

            loop_ring.hashes[loop_ring.count % TOOL_LOOP_RING_SIZE] = h;
            loop_ring.count++;

            /* T118: truncate at write time, spill full output to temp file */
            char *stored = truncate_and_spill(result, ctx->session_id,
                                             asst.tool_calls[i].id);
            ToolResult tr = {.tool_call_id = asst.tool_calls[i].id,
                             .content = stored ? stored : result};
            int err = (result[0] == 'e' && strncmp(result, "error:", 6) == 0);
            Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                .tool_name = asst.tool_calls[i].name, .is_error = err};
            entry_append_with_turn(ctx->db, ctx->session_id, &tool_msg, turn_id);
            free(stored);
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
        free(asst.metadata_json);
        arena_destroy(a);

        /* V43: break agent loop on tool loop detection */
        if (loop_break) return -1;
    }

    /* Max iterations reached */
    return -1;
}
