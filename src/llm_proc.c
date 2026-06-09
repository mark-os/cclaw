#define _POSIX_C_SOURCE 200809L
#include "llm_proc.h"
#include "agent_setup.h"
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

/* ── turn_complete: DB-based completion detection ──────────────── */

int turn_complete(sqlite3 *db, int64_t session_id) {
    int tc_count = 0;
    PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
    if (calls) {
        db_tool_call_free_pending(calls, tc_count);
        if (tc_count > 0) return 1;  /* has pending tool_calls */
    }
    /* Check last entry's stop_reason */
    const char *sql = "SELECT stop_reason FROM entries WHERE session_id=?"
                      " AND role=2 ORDER BY id DESC LIMIT 1;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    int result = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *sr = (const char *)sqlite3_column_text(stmt, 0);
        if (sr && strcmp(sr, "error") == 0) result = -1;
    }
    sqlite3_finalize(stmt);
    return result;
}

/* ── llm_req: single LLM HTTP call ────────────────────────────── */

int llm_req(sqlite3 *db, CURL *curl, int64_t session_id, int recall) {
    Config *cfg = config_load(db);
    if (!cfg) { fprintf(stderr, "llm_req: config load failed\n"); return -1; }

    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    if (!a) { config_free(cfg); return -1; }

    /* Load tool schemas */
    const char *agent_name = getenv("CCLAW_AGENT_NAME");
    if (!agent_name || !agent_name[0]) agent_name = "default";

    AgentSetup setup;
    agent_setup_init(&setup, db, session_id, cfg, agent_name, NULL, 0, AGENT_SETUP_CLI);

    ToolSchema schemas[TOOLS_MAX];
    size_t tool_count = agent_setup_schemas(&setup, schemas, TOOLS_MAX);

    /* Tool overhead for context plan */
    int tool_overhead = 0;
    for (size_t i = 0; i < tool_count; i++) {
        tool_overhead += 4;
        if (schemas[i].name) tool_overhead += (int)strlen(schemas[i].name) / 4;
        if (schemas[i].description) tool_overhead += (int)strlen(schemas[i].description) / 4;
        if (schemas[i].parameters_json) tool_overhead += (int)strlen(schemas[i].parameters_json) / 4;
    }

    /* Context planning */
    ContextPlan plan = {0};
    if (context_plan(db, session_id, cfg, tool_overhead, &plan) != 0) {
        LOG_DEBUG(cfg, "llm_req: context_plan failed");
        goto err;
    }
    LOG_DEBUG(cfg, "llm_req: %d entries, cut=%d", plan.count, plan.cut);

    /* Auto-recall */
    char *recall_text = NULL;
    if (cfg->auto_recall && recall) {
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
                                              &plan, schemas, tool_count,
                                              &resp, llm_sse_stdout_cb, NULL, &sse_ctx,
                                              recall_text, gcache, NULL, curl);
        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                          (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
        last_status = status;
        LOG_DEBUG(cfg, "llm_req: %ldms status=%d ttfb=%.3fs tls=%.3fs%s",
                  elapsed_ms, status, resp.ttfb, resp.tls_time,
                  resp.conn_reused ? " conn_reused" : "");

        if (cfg->log_level >= LOG_LEVEL_TRACE && resp.data)
            LOG_TRACE(cfg, "RESP status=%d %s", status, resp.data);

        /* Context overflow */
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

    /* Write response to DB */
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
        LOG_DEBUG(cfg, "llm_req: db_ingest_typed failed");
        goto err;
    }
    free(ir.tc_entry_ids);

    agent_setup_destroy(&setup);
    arena_destroy(a);
    config_free(cfg);
    return 0;

err:
    context_plan_free(&plan);
    agent_setup_destroy(&setup);
    arena_destroy(a);
    config_free(cfg);
    return -1;
}

/* ── llm_proc_main: fork-mode entry point ──────────────────────── */

int llm_proc_main(int64_t session_id) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    shutdown_init();
    cclaw_log_init();

    const char *db_path = getenv("CCLAW_DB");
    if (!db_path || !db_path[0]) {
        syslog(LOG_ERR, "llm_proc_main: CCLAW_DB not set");
        return LLM_EXIT_ERROR;
    }

    sqlite3 *db = db_open(db_path);
    if (!db) return LLM_EXIT_ERROR;
    db_set_child_pragmas(db);

    int recall = 0;
    const char *recall_env = getenv("CCLAW_RECALL");
    if (recall_env && recall_env[0] == '1') recall = 1;

    int rc = llm_req(db, NULL, session_id, recall);
    if (rc != 0) {
        db_close(db);
        return LLM_EXIT_ERROR;
    }

    /* Determine exit code from DB state for backward compat */
    int tc = turn_complete(db, session_id);
    db_close(db);
    return (tc == 1) ? LLM_EXIT_TOOLCALL : LLM_EXIT_STOP;
}
