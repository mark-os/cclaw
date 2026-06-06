#define _POSIX_C_SOURCE 200809L
#include "agent.h"
#include "agent_exit.h"
#include "context.h"
#include "gemini_cache.h"
#include "hook_dispatch.h"
#include "llm_transport.h"
#include "log.h"
#include "request_stream.h"
#include "http.h"
#include "shutdown.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* V32: max LLM error retries per turn (parse failure, missing finish_reason) */
#define MAX_LLM_RETRIES 3

/* V10: dispatch a single tool call, always returns a result string */
static char *dispatch_tool(AgentContext *ctx, const ToolCall *tc) {
    if (!ctx->dispatch) {
        char *err = malloc(64);
        if (err) snprintf(err, 64, "error: no tool dispatcher registered");
        return err;
    }

    /* V113/T259: beforeToolCall hook — can block execution */
    char *blocked = hook_dispatch_before_tool_call(ctx->ext_ctx, tc->name, tc->arguments);
    if (blocked) return blocked;

    /* T115: notify progress — tool starting */
    if (ctx->progress)
        ctx->progress(PROGRESS_TOOL_START, tc->name, tc->arguments, ctx->progress_data);

    char *result = ctx->dispatch(tc->name, tc->arguments, ctx->dispatch_data);
    if (!result) {
        result = malloc(64);
        if (result) snprintf(result, 64, "error: tool '%s' returned null", tc->name);
    }

    /* V114/T259: afterToolCall hook — can replace result */
    char *replaced = hook_dispatch_after_tool_call(ctx->ext_ctx, tc->name, tc->arguments, result);
    if (replaced) {
        free(result);
        return replaced;
    }

    return result;
}

/* T45,V41: call LLM via shared transport (includes retry + fallback chain) */
static int llm_call_with_fallback_stream(Arena *a, const Config *cfg,
                                         sqlite3 *db, int64_t session_id,
                                         const ContextPlan *plan,
                                         const ToolSchema *tools, size_t tool_count,
                                         const char *body_override,
                                         const char *recall_text,
                                         const char *gemini_cache_name,
                                         HttpResponse *resp, SseCtx *out_ctx) {
    return llm_call_with_fallbacks(a, db, session_id, cfg, plan, tools, tool_count,
                                   resp, llm_sse_stdout_cb, NULL, out_ctx,
                                   recall_text, gemini_cache_name, body_override);
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

/* V87/T224: Partial-turn resume — execute remaining tool_calls from an
 * incomplete batch (daemon resolved PENDING, agent re-forked with remaining
 * calls unexecuted). Returns: 0 = nothing to resume, positive = sentinel exit,
 * -1 = error, -2 = resumed successfully (continue to LLM loop). */
static int partial_turn_resume(AgentContext *ctx) {
    int branch_count = 0;
    Entry *branch = session_get_branch(ctx->db, ctx->session_id, &branch_count);
    if (!branch || branch_count == 0) {
        entry_branch_free(branch, branch_count);
        return 0;
    }

    /* Find last assistant entry with tool_calls at the tail */
    int last_asst = -1;
    for (int i = branch_count - 1; i >= 0; i--) {
        if (branch[i].message.role == ROLE_ASSISTANT && branch[i].message.tool_calls) {
            last_asst = i;
            break;
        }
        if (branch[i].message.role == ROLE_USER) break;
    }
    if (last_asst < 0) {
        entry_branch_free(branch, branch_count);
        return 0;
    }

    /* Collect existing tool_results after this assistant */
    int results_found = 0;
    int has_pending = 0;
    for (int i = last_asst + 1; i < branch_count; i++) {
        if (branch[i].message.role != ROLE_TOOL) break;
        results_found++;
        if (branch[i].message.tool_result && branch[i].message.tool_result->content &&
            strcmp(branch[i].message.tool_result->content, "PENDING") == 0)
            has_pending = 1;
    }

    int expected = (int)branch[last_asst].message.tool_call_count;
    if (results_found >= expected && !has_pending) {
        /* All results present, no PENDING — nothing to resume */
        entry_branch_free(branch, branch_count);
        return 0;
    }

    /* V17 case 1: PENDING entry with no resolution → replace with error */
    if (has_pending) {
        const char *upd_sql =
            "UPDATE entries SET content='error: daemon did not deliver result'"
            " WHERE session_id=? AND content='PENDING' AND role=3;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(ctx->db, upd_sql, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, ctx->session_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        entry_branch_free(branch, branch_count);
        return -2; /* resumed — continue to LLM loop */
    }

    /* V87: results missing (no PENDING) → execute remaining tool_calls */
    /* Build set of tool_call_ids that already have results */
    const ToolCall *tool_calls = branch[last_asst].message.tool_calls;

    /* Collect IDs of existing results */
    char **resolved_ids = NULL;
    if (results_found > 0) {
        resolved_ids = malloc((size_t)results_found * sizeof(char *));
        int ri = 0;
        for (int i = last_asst + 1; i < branch_count && ri < results_found; i++) {
            if (branch[i].message.role != ROLE_TOOL) break;
            if (branch[i].message.tool_result && branch[i].message.tool_result->tool_call_id)
                resolved_ids[ri] = branch[i].message.tool_result->tool_call_id;
            else
                resolved_ids[ri] = NULL;
            ri++;
        }
    }

    int64_t turn_id = db_next_turn_id(ctx->db, ctx->session_id);
    int sentinel_exit = -1;

    for (int i = 0; i < expected; i++) {
        /* Skip if this tool_call already has a result */
        int already_resolved = 0;
        for (int r = 0; r < results_found; r++) {
            if (resolved_ids && resolved_ids[r] && tool_calls[i].id &&
                strcmp(resolved_ids[r], tool_calls[i].id) == 0) {
                already_resolved = 1;
                break;
            }
        }
        if (already_resolved) continue;

        /* Dispatch this tool_call */
        char *result = dispatch_tool(ctx, &tool_calls[i]);

        /* Check for sentinel exit codes */
        if (strncmp(result, SENTINEL_SPAWN, strlen(SENTINEL_SPAWN)) == 0)
            sentinel_exit = AGENT_EXIT_SPAWN;
        else if (strncmp(result, SENTINEL_APPROVAL, strlen(SENTINEL_APPROVAL)) == 0)
            sentinel_exit = AGENT_EXIT_APPROVAL;
        else if (strncmp(result, SENTINEL_CONFIG, strlen(SENTINEL_CONFIG)) == 0)
            sentinel_exit = AGENT_EXIT_CONFIG;

        if (ctx->progress)
            ctx->progress(PROGRESS_TOOL_RESULT, tool_calls[i].name, result, ctx->progress_data);

        if (sentinel_exit >= 0) {
            ToolResult tr = {.tool_call_id = tool_calls[i].id, .content = "PENDING"};
            Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                .tool_name = tool_calls[i].name};
            entry_append_with_turn(ctx->db, ctx->session_id, &tool_msg, turn_id);
            free(result);
            free(resolved_ids);
            entry_branch_free(branch, branch_count);
            return sentinel_exit;
        }

        char *stored = truncate_and_spill(result, ctx->session_id, tool_calls[i].id);
        ToolResult tr = {.tool_call_id = tool_calls[i].id,
                         .content = stored ? stored : result};
        int err = (result[0] == 'e' && strncmp(result, "error:", 6) == 0);
        Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                            .tool_name = tool_calls[i].name, .is_error = err};
        entry_append_with_turn(ctx->db, ctx->session_id, &tool_msg, turn_id);
        free(stored);
        free(result);
    }

    free(resolved_ids);
    entry_branch_free(branch, branch_count);
    return -2; /* resumed — continue to LLM loop */
}

int agent_run(AgentContext *ctx) {
    if (!ctx || !ctx->db || !ctx->cfg) return -1;

    /* V87/T224: Check for incomplete tool_call batch and resume */
    int resume_rc = partial_turn_resume(ctx);
    if (resume_rc > 0) return resume_rc; /* sentinel exit (spawn/approval/config) */
    if (resume_rc == -1) return -1;      /* error */
    /* resume_rc == 0 or -2: proceed to LLM loop */

    /* T260/V111: turnStart hook — informational */
    hook_dispatch_turn_start(ctx->ext_ctx);

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
        int e1_retries = 0; /* V94: zero-usage retry counter */
        int last_status = 0; /* T236: track last HTTP status for error message */

        for (int retry = 0; retry < MAX_LLM_RETRIES; retry++) {
            /* V41: plan entry IDs + cut point from SQLite (pass 1, no content loaded) */
            ContextPlan plan;
            /* Estimate tokens consumed by tool definitions (not stored as entries) */
            int tool_overhead = 0;
            for (size_t ti = 0; ti < ctx->tool_count; ti++) {
                tool_overhead += 4; /* per-tool framing */
                if (ctx->tools[ti].name) tool_overhead += (int)strlen(ctx->tools[ti].name) / 4;
                if (ctx->tools[ti].description) tool_overhead += (int)strlen(ctx->tools[ti].description) / 4;
                if (ctx->tools[ti].parameters_json) tool_overhead += (int)strlen(ctx->tools[ti].parameters_json) / 4;
            }
            int rc = context_plan(ctx->db, ctx->session_id, ctx->cfg, tool_overhead, &plan);
            if (rc != 0) {
                arena_destroy(a);
                return -1;
            }
            LOG_DEBUG(ctx->cfg, "context_plan: %d entries, cut=%d, budget=%d (overhead=%d)",
                      plan.count, plan.cut, plan.budget, tool_overhead);

            /* V94: after 2 E1 retries on primary, try fallback if available */
            const Config *call_cfg = ctx->cfg;
            Config fb_cfg;
            if (e1_retries >= 2 && ctx->cfg->fallback_count > 0) {
                fb_cfg = *ctx->cfg;
                fb_cfg.provider = ctx->cfg->fallback_providers[0];
                call_cfg = &fb_cfg;
                LOG_DEBUG(ctx->cfg, "E1 zero-usage: trying fallback model");
            }

            /* T45,V41: call LLM with streaming request + fallback chain */
            /* T269: auto-recall — compute recalled context from FTS5 */
            char *recall_text = NULL;
            if (ctx->cfg->auto_recall && iter == 0) {
                /* Get last user message content for recall query */
                const char *uq = "SELECT content FROM entries WHERE session_id=? AND role=1"
                                 " ORDER BY id DESC LIMIT 1;";
                sqlite3_stmt *ust;
                if (sqlite3_prepare_v2(ctx->db, uq, -1, &ust, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(ust, 1, ctx->session_id);
                    if (sqlite3_step(ust) == SQLITE_ROW) {
                        const char *umsg = (const char *)sqlite3_column_text(ust, 0);
                        if (umsg)
                            recall_text = context_auto_recall(ctx->db, ctx->session_id,
                                                             umsg, ctx->cfg->recall_max_tokens);
                    }
                    sqlite3_finalize(ust);
                }
            }

            /* T258/V112: beforeRequest hook — may provide modified request body */
            char *body_override = hook_dispatch_before_request(
                ctx->ext_ctx, ctx->db, ctx->session_id, call_cfg,
                &plan, ctx->tools, ctx->tool_count);

            HttpResponse resp = {0};
            SseCtx sse_ctx = {0};
            struct timespec t_start;
            clock_gettime(CLOCK_MONOTONIC, &t_start);

            /* T291: Gemini context caching — get or create cached content */
            char *gcache = NULL;
            if (call_cfg->provider.endpoint_type == ENDPOINT_GEMINI &&
                call_cfg->provider.cache_hints != CACHE_HINTS_OFF)
                gcache = gemini_cache_get_or_create(ctx->db, ctx->session_id,
                                                   call_cfg, &plan);

            int status = llm_call_with_fallback_stream(a, call_cfg, ctx->db,
                                                       ctx->session_id, &plan,
                                                       ctx->tools, ctx->tool_count,
                                                       body_override,
                                                       recall_text,
                                                       gcache,
                                                       &resp, &sse_ctx);
            free(gcache);
            free(body_override);
            free(recall_text);
            struct timespec t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_end);
            long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                              (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
            context_plan_free(&plan);

            LOG_DEBUG(ctx->cfg, "LLM call: %ldms, status=%d", elapsed_ms, status);
            last_status = status;

            if (ctx->cfg->log_level >= LOG_LEVEL_TRACE && resp.data)
                LOG_TRACE(ctx->cfg, "RESP status=%d %s", status, resp.data);

            /* Context overflow detection (E5) — return error so caller can trim */
            if (status == 400 && llm_is_context_overflow(resp.data)) {
                LOG_DEBUG(ctx->cfg, "E5: context overflow detected");
                http_response_free(&resp);
                sse_ctx_free(&sse_ctx);
                arena_destroy(a);
                return -2;
            }

            /* T236: Error classification — non-retryable failures break immediately */
            if (status == -2) {
                LOG_DEBUG(ctx->cfg, "E10: request timed out");
                http_response_free(&resp);
                sse_ctx_free(&sse_ctx);
                break;
            }
            if (status == -1) {
                LOG_DEBUG(ctx->cfg, "E4: network error");
                http_response_free(&resp);
                sse_ctx_free(&sse_ctx);
                break;
            }
            if (status == 401 || status == 403) {
                LOG_DEBUG(ctx->cfg, "E11: auth failure (%d)", status);
                http_response_free(&resp);
                sse_ctx_free(&sse_ctx);
                break;
            }
            if (status == 404) {
                LOG_DEBUG(ctx->cfg, "E12: model not found");
                http_response_free(&resp);
                sse_ctx_free(&sse_ctx);
                break;
            }
            if (status == 429) {
                LOG_DEBUG(ctx->cfg, "E2: rate limit exhausted after retries");
                http_response_free(&resp);
                sse_ctx_free(&sse_ctx);
                break;
            }

            /* V127: 5xx already exhausted inner retry+fallback chain — break, ⊥ re-retry */
            if (status >= 500 && status < 600) {
                LOG_DEBUG(ctx->cfg, "E3: HTTP %d exhausted after inner retries",
                          status);
                http_response_free(&resp);
                sse_ctx_free(&sse_ctx);
                break;
            }

            /* Other unexpected HTTP errors (3xx, no data) — retry at outer level */
            if (status < 200 || status >= 300 || !resp.data) {
                LOG_DEBUG(ctx->cfg, "E3: HTTP %d, retry %d/%d",
                          status, retry + 1, MAX_LLM_RETRIES);
                http_response_free(&resp);
                sse_ctx_free(&sse_ctx); memset(&sse_ctx, 0, sizeof(sse_ctx));
                continue;
            }

            /* V10/V29: JSON parse failure → retry */
            /* T290: use Gemini parser for native Gemini responses (non-streaming only;
             * streaming uses llm_response_from_sse directly) */
            if (call_cfg->stream)
                rc = llm_response_from_sse(a, &sse_ctx, &llm_resp);
            else if (call_cfg->provider.endpoint_type == ENDPOINT_GEMINI)
                rc = llm_parse_response_gemini(a, resp.data, &llm_resp);
            else
                rc = llm_parse_response(a, resp.data, &llm_resp);
            http_response_free(&resp);
            sse_ctx_free(&sse_ctx);
            if (rc != 0) {
                LOG_DEBUG(ctx->cfg, "JSON parse failure, retry %d/%d",
                          retry + 1, MAX_LLM_RETRIES);
                continue;
            }

            /* V29: missing finish_reason → treat as error, retry */
            if (!llm_resp.finish_reason) {
                LOG_DEBUG(ctx->cfg, "missing finish_reason, retry %d/%d",
                          retry + 1, MAX_LLM_RETRIES);
                continue;
            }

            /* T235: log response shape at debug level */
            LOG_DEBUG(ctx->cfg, "response: finish=%s prompt=%d completion=%d total=%d tools=%zu content=%s",
                      llm_resp.finish_reason,
                      llm_resp.usage.prompt_tokens,
                      llm_resp.usage.completion_tokens,
                      llm_resp.usage.total_tokens,
                      llm_resp.tool_call_count,
                      llm_resp.content ? "yes" : "null");

            /* E9: content filter — non-retryable, write error entry */
            if (strcmp(llm_resp.finish_reason, "content_filter") == 0) {
                LOG_DEBUG(ctx->cfg, "E9: content filter triggered");
                Message err_msg = {.role = ROLE_ASSISTANT,
                                   .content = strdup("error: response filtered by provider safety policy"),
                                   .stop_reason = STOP_REASON_ERROR,
                                   .model = ctx->cfg->provider.model};
                entry_append_with_turn(ctx->db, ctx->session_id, &err_msg, turn_id);
                free(err_msg.content);
                arena_destroy(a);
                return -1;
            }

            /* E8: token limit exhaustion — store partial content as-is */
            if (strcmp(llm_resp.finish_reason, "length") == 0) {
                LOG_DEBUG(ctx->cfg, "E8: finish_reason=length, storing partial");
                char *meta = build_metadata(ctx->cfg, &llm_resp);
                Message asst = {.role = ROLE_ASSISTANT,
                                .content = llm_resp.content ? strdup(llm_resp.content) : strdup(""),
                                .stop_reason = STOP_REASON_LENGTH,
                                .metadata_json = meta,
                                .model = ctx->cfg->provider.model,
                                .usage_in = llm_resp.usage.prompt_tokens,
                                .usage_out = llm_resp.usage.completion_tokens,
                                .cost_nano = llm_resp.usage.cost_nano};
                entry_append_with_turn(ctx->db, ctx->session_id, &asst, turn_id);
                free(asst.content);
                free(meta);
                arena_destroy(a);
                return 0;
            }

            /* V94/T231: zero-usage empty stop (E1) — provider glitch, retry */
            if (llm_resp.usage.total_tokens == 0 &&
                (!llm_resp.content || !llm_resp.content[0]) &&
                strcmp(llm_resp.finish_reason, "stop") == 0) {
                e1_retries++;
                LOG_DEBUG(ctx->cfg, "E1 zero-usage empty stop, e1_retry %d", e1_retries);
                if (e1_retries <= 2 || (e1_retries == 3 && ctx->cfg->fallback_count > 0))
                    continue;
                /* Exhausted — fall through to write error entry below */
                break;
            }

            llm_ok = 1;
            break;
        }

        /* T261/V111: afterResponse hook — read-only inspect */
        hook_dispatch_after_response(ctx->ext_ctx, llm_resp.content,
                                     llm_resp.finish_reason,
                                     (int)llm_resp.tool_call_count);

        /* V32: all retries exhausted → write final error entry + exit */
        if (!llm_ok) {
            const char *err_text;
            if (e1_retries > 0)
                err_text = "error: model returned empty response — provider glitch, try again";
            else if (last_status == -2)
                err_text = "error: request timed out";
            else if (last_status == -1)
                err_text = "error: network error — could not reach provider";
            else if (last_status == 401 || last_status == 403)
                err_text = "error: authentication failed — check API key";
            else if (last_status == 404)
                err_text = "error: model not available";
            else if (last_status == 429)
                err_text = "error: rate limited by provider";
            else if (last_status >= 500)
                err_text = "error: provider server error";
            else
                err_text = "error: LLM request failed after retries";
            Message err_msg = {.role = ROLE_ASSISTANT,
                               .content = strdup(err_text),
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
            char *meta = build_metadata(ctx->cfg, &llm_resp);
            Message asst = {.role = ROLE_ASSISTANT,
                            .content = llm_resp.content ? strdup(llm_resp.content) : strdup(""),
                            .stop_reason = sr,
                            .metadata_json = meta,
                            .model = ctx->cfg->provider.model,
                            .usage_in = llm_resp.usage.prompt_tokens,
                            .usage_out = llm_resp.usage.completion_tokens,
                            .cost_nano = llm_resp.usage.cost_nano};
            entry_append_with_turn(ctx->db, ctx->session_id, &asst, turn_id);

            free(asst.content);
            free(meta);
            arena_destroy(a);
            hook_dispatch_turn_end(ctx->ext_ctx);
            return 0;
        }

        /* Has tool calls — append assistant message with tool_calls */
        Message asst = {0};
        asst.role = ROLE_ASSISTANT;
        asst.content = llm_resp.content ? strdup(llm_resp.content) : NULL;
        asst.stop_reason = map_stop_reason(llm_resp.finish_reason);
        asst.tool_call_count = llm_resp.tool_call_count;
        asst.metadata_json = build_metadata(ctx->cfg, &llm_resp);
        asst.model = ctx->cfg->provider.model;
        asst.usage_in = llm_resp.usage.prompt_tokens;
        asst.usage_out = llm_resp.usage.completion_tokens;
        asst.cost_nano = llm_resp.usage.cost_nano;

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

        /* Dispatch each tool call and append results (V10) */
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

            char *result = dispatch_tool(ctx, &asst.tool_calls[i]);

            /* V72/T197: detect exit-code sentinels from tool results */
            int sentinel_exit = -1;
            if (strncmp(result, SENTINEL_SPAWN, strlen(SENTINEL_SPAWN)) == 0)
                sentinel_exit = AGENT_EXIT_SPAWN;
            else if (strncmp(result, SENTINEL_APPROVAL, strlen(SENTINEL_APPROVAL)) == 0)
                sentinel_exit = AGENT_EXIT_APPROVAL;
            else if (strncmp(result, SENTINEL_CONFIG, strlen(SENTINEL_CONFIG)) == 0)
                sentinel_exit = AGENT_EXIT_CONFIG;

            /* T115: notify progress — tool result (truncated for display) */
            if (ctx->progress)
                ctx->progress(PROGRESS_TOOL_RESULT, asst.tool_calls[i].name,
                              result, ctx->progress_data);

            /* V72/V13/T201: sentinel → store "PENDING" as content, exit */
            if (sentinel_exit >= 0) {
                ToolResult tr = {.tool_call_id = asst.tool_calls[i].id,
                                 .content = "PENDING"};
                Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                    .tool_name = asst.tool_calls[i].name};
                entry_append_with_turn(ctx->db, ctx->session_id, &tool_msg, turn_id);
                free(result);

                for (size_t j = 0; j < asst.tool_call_count; j++) {
                    free(asst.tool_calls[j].id);
                    free(asst.tool_calls[j].name);
                    free(asst.tool_calls[j].arguments);
                }
                free(asst.tool_calls);
                free(asst.content);
                free(asst.metadata_json);
                arena_destroy(a);
                return sentinel_exit;
            }

            /* T118: truncate at write time, spill full output to temp file */
            char *stored = truncate_and_spill(result, ctx->session_id,
                                             asst.tool_calls[i].id);
            ToolResult tr = {.tool_call_id = asst.tool_calls[i].id,
                             .content = stored ? stored : (result ? result : "error: tool returned null")};
            int err = (result && result[0] == 'e' && strncmp(result, "error:", 6) == 0);
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
    }

    /* Max iterations reached */
    hook_dispatch_turn_end(ctx->ext_ctx);
    return -1;
}
