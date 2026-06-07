#define _POSIX_C_SOURCE 200809L
#include "llm_proc.h"
#include "config.h"
#include "context.h"
#include "db.h"
#include "db_response.h"
#include "gemini_cache.h"
#include "http.h"
#include "llm.h"
#include "llm_transport.h"
#include "log.h"
#include "request_stream.h"
#include "shutdown.h"
#include "arena.h"
#include "cJSON.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define MAX_LLM_RETRIES 3

int llm_proc_main(int64_t session_id) {
    /* V34: die if parent dies */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    shutdown_init();

    Config *cfg = config_load_from_env();
    if (!cfg) { fprintf(stderr, "llm: config load failed\n"); return LLM_EXIT_ERROR; }

    const char *db_path = getenv("CCLAW_DB");
    if (!db_path || !db_path[0]) db_path = cfg->db_path;

    sqlite3 *db = db_open(db_path);
    if (!db) { config_free(cfg); return LLM_EXIT_ERROR; }
    db_set_child_pragmas(db);

    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    if (!a) { db_close(db); config_free(cfg); return LLM_EXIT_ERROR; }

    /* Read tool schemas from env-specified tool list (CCLAW_TOOLS_JSON) */
    const char *tools_json_env = getenv("CCLAW_TOOLS_JSON");
    ToolSchema *tools = NULL;
    size_t tool_count = 0;

    if (tools_json_env && tools_json_env[0]) {
        /* Parse JSON array of {name, description, parameters} */
        cJSON *arr = cJSON_Parse(tools_json_env);
        if (arr && cJSON_IsArray(arr)) {
            tool_count = (size_t)cJSON_GetArraySize(arr);
            tools = calloc(tool_count, sizeof(ToolSchema));
            if (tools) {
                for (size_t i = 0; i < tool_count; i++) {
                    cJSON *item = cJSON_GetArrayItem(arr, (int)i);
                    cJSON *n = cJSON_GetObjectItem(item, "name");
                    cJSON *d = cJSON_GetObjectItem(item, "description");
                    cJSON *p = cJSON_GetObjectItem(item, "parameters");
                    if (n) tools[i].name = strdup(n->valuestring);
                    if (d) tools[i].description = strdup(d->valuestring);
                    if (p) tools[i].parameters_json = cJSON_PrintUnformatted(p);
                }
            }
        }
        cJSON_Delete(arr);
    }

    /* Tool overhead for context plan */
    int tool_overhead = 0;
    for (size_t i = 0; i < tool_count; i++) {
        tool_overhead += 4;
        if (tools[i].name) tool_overhead += (int)strlen(tools[i].name) / 4;
        if (tools[i].description) tool_overhead += (int)strlen(tools[i].description) / 4;
        if (tools[i].parameters_json) tool_overhead += (int)strlen(tools[i].parameters_json) / 4;
    }

    /* Context planning */
    ContextPlan plan = {0};
    if (context_plan(db, session_id, cfg, tool_overhead, &plan) != 0) {
        LOG_DEBUG(cfg, "llm: context_plan failed");
        goto err;
    }
    LOG_DEBUG(cfg, "llm: context_plan: %d entries, cut=%d", plan.count, plan.cut);

    /* Auto-recall (first call only — parent signals via env) */
    char *recall_text = NULL;
    if (cfg->auto_recall) {
        const char *recall_env = getenv("CCLAW_RECALL");
        if (recall_env && recall_env[0] == '1') {
            const char *uq = "SELECT content FROM entries WHERE session_id=? AND role=1"
                             " ORDER BY id DESC LIMIT 1;";
            sqlite3_stmt *ust;
            if (sqlite3_prepare_v2(db, uq, -1, &ust, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(ust, 1, session_id);
                if (sqlite3_step(ust) == SQLITE_ROW) {
                    const char *umsg = (const char *)sqlite3_column_text(ust, 0);
                    if (umsg)
                        recall_text = context_auto_recall(db, session_id, umsg,
                                                         cfg->recall_max_tokens);
                }
                sqlite3_finalize(ust);
            }
        }
    }

    /* Gemini cache */
    char *gcache = NULL;
    if (cfg->provider.endpoint_type == ENDPOINT_GEMINI &&
        cfg->provider.cache_hints != CACHE_HINTS_OFF)
        gcache = gemini_cache_get_or_create(db, session_id, cfg, &plan);

    /* LLM call with retry loop */
    int llm_ok = 0;
    LlmResponse llm_resp;
    memset(&llm_resp, 0, sizeof(llm_resp));
    int e1_retries = 0;
    int last_status = 0;

    for (int retry = 0; retry < MAX_LLM_RETRIES; retry++) {
        const Config *call_cfg = cfg;
        Config fb_cfg;
        if (e1_retries >= 2 && cfg->fallback_count > 0) {
            fb_cfg = *cfg;
            fb_cfg.provider = cfg->fallback_providers[0];
            call_cfg = &fb_cfg;
            LOG_DEBUG(cfg, "E1 zero-usage: trying fallback model");
        }

        HttpResponse resp = {0};
        SseCtx sse_ctx = {0};
        struct timespec t_start;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        int status = llm_call_with_fallbacks(a, db, session_id, call_cfg,
                                              &plan, tools, tool_count,
                                              &resp, llm_sse_stdout_cb, NULL, &sse_ctx,
                                              recall_text, gcache, NULL);
        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                          (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
        last_status = status;
        LOG_DEBUG(cfg, "llm: %ldms, status=%d", elapsed_ms, status);

        if (cfg->log_level >= LOG_LEVEL_TRACE && resp.data)
            LOG_TRACE(cfg, "RESP status=%d %s", status, resp.data);

        /* Context overflow → exit 1 (parent handles) */
        if (status == 400 && llm_is_context_overflow(resp.data)) {
            LOG_DEBUG(cfg, "E5: context overflow");
            http_response_free(&resp);
            sse_ctx_free(&sse_ctx);
            goto err;
        }

        /* Non-retryable failures */
        if (status == -2 || status == -1 || status == 401 || status == 403 ||
            status == 404 || status == 429 || (status >= 500 && status < 600)) {
            http_response_free(&resp);
            sse_ctx_free(&sse_ctx);
            break;
        }

        if (status < 200 || status >= 300 || !resp.data) {
            http_response_free(&resp);
            sse_ctx_free(&sse_ctx);
            continue;
        }

        /* Parse response */
        int rc;
        if (call_cfg->stream)
            rc = llm_response_from_sse(a, &sse_ctx, &llm_resp);
        else if (call_cfg->provider.endpoint_type == ENDPOINT_GEMINI)
            rc = llm_parse_response_gemini(a, resp.data, &llm_resp);
        else
            rc = llm_parse_response(a, resp.data, &llm_resp);
        http_response_free(&resp);
        sse_ctx_free(&sse_ctx);

        if (rc != 0) { LOG_DEBUG(cfg, "parse failure, retry"); continue; }
        if (!llm_resp.finish_reason) { LOG_DEBUG(cfg, "missing finish_reason, retry"); continue; }

        /* V94: zero-usage empty stop */
        if (llm_resp.usage.total_tokens == 0 &&
            (!llm_resp.content || !llm_resp.content[0]) &&
            strcmp(llm_resp.finish_reason, "stop") == 0) {
            e1_retries++;
            LOG_DEBUG(cfg, "E1 zero-usage, retry %d", e1_retries);
            if (e1_retries <= 2 || (e1_retries == 3 && cfg->fallback_count > 0))
                continue;
            break;
        }

        llm_ok = 1;
        break;
    }

    free(gcache);
    free(recall_text);
    context_plan_free(&plan);

    if (!llm_ok) {
        /* Write error entry */
        int64_t turn_id = db_next_turn_id(db, session_id);
        const char *err_text = e1_retries > 0
            ? "error: model returned empty response — provider glitch"
            : last_status == -2 ? "error: request timed out"
            : last_status == -1 ? "error: network error"
            : last_status == 401 || last_status == 403 ? "error: authentication failed"
            : last_status == 404 ? "error: model not available"
            : last_status == 429 ? "error: rate limited"
            : last_status >= 500 ? "error: provider server error"
            : "error: LLM request failed after retries";
        Message err_msg = {.role = ROLE_ASSISTANT, .content = (char *)err_text,
                           .stop_reason = STOP_REASON_ERROR,
                           .model = cfg->provider.model};
        entry_append_with_turn(db, session_id, &err_msg, turn_id);
        goto err;
    }

    /* Write response to DB using typed entries */
    int64_t turn_id = db_next_turn_id(db, session_id);

    const char **tc_ids = NULL, **tc_names = NULL, **tc_args = NULL;
    int tc_count = (int)llm_resp.tool_call_count;
    if (tc_count > 0) {
        tc_ids = malloc((size_t)tc_count * sizeof(char *));
        tc_names = malloc((size_t)tc_count * sizeof(char *));
        tc_args = malloc((size_t)tc_count * sizeof(char *));
        for (int i = 0; i < tc_count; i++) {
            tc_ids[i] = llm_resp.tool_calls[i].id;
            tc_names[i] = llm_resp.tool_calls[i].name;
            tc_args[i] = llm_resp.tool_calls[i].arguments;
        }
    }

    TypedIngestResult ir;
    int rc = db_ingest_typed(db, session_id, turn_id,
                             cfg->provider.model,
                             llm_resp.content, llm_resp.reasoning,
                             llm_resp.finish_reason,
                             llm_resp.usage.prompt_tokens,
                             llm_resp.usage.completion_tokens,
                             llm_resp.usage.cost_nano,
                             tc_ids, tc_names, tc_args, tc_count, &ir);
    free(tc_ids);
    free(tc_names);
    free(tc_args);

    if (rc != 0) {
        LOG_DEBUG(cfg, "llm: db_ingest_typed failed");
        goto err;
    }
    free(ir.tc_entry_ids);

    int exit_code = (tc_count > 0) ? LLM_EXIT_TOOLCALL : LLM_EXIT_STOP;

    /* Clean exit */
    for (size_t i = 0; i < tool_count; i++) {
        free((void *)tools[i].name);
        free((void *)tools[i].description);
        free((void *)tools[i].parameters_json);
    }
    free(tools);
    arena_destroy(a);
    db_close(db);
    config_free(cfg);
    return exit_code;

err:
    context_plan_free(&plan);
    for (size_t i = 0; i < tool_count; i++) {
        free((void *)tools[i].name);
        free((void *)tools[i].description);
        free((void *)tools[i].parameters_json);
    }
    free(tools);
    arena_destroy(a);
    db_close(db);
    config_free(cfg);
    return LLM_EXIT_ERROR;
}
