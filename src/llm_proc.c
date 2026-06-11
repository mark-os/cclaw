#define _POSIX_C_SOURCE 200809L
#include "llm_proc.h"
#include "agent_config.h"
#include "agent_setup.h"
#include "config.h"
#include "context.h"
#include "db.h"
#include "db_response.h"

#include "http.h"
#include "llm.h"
#include "llm_payload.h"
#include "llm_transport.h"
#include "log.h"
#include "shutdown.h"
#include "arena.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <time.h>
#include <unistd.h>

#define MAX_MODELS 16
#define MAX_429_RETRIES 3
#define MAX_PARSE_RETRIES 3

/* ── turn_complete: DB-based completion detection ──────────────── */

int turn_complete(sqlite3 *db, int64_t session_id) {
    int tc_count = 0;
    PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
    if (calls) {
        db_tool_call_free_pending(calls, tc_count);
        if (tc_count > 0) return 1;
    }
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

/* ── Model candidate (loaded from DB) ──────────────────────────── */

typedef struct {
    char id[128];
    char model[128];
    char base_url[256];
    char api_key_env[64];
    int endpoint_type;
    int context_window;
} ModelCandidate;

static int load_candidates(sqlite3 *db, ModelCandidate *out, int max) {
    const char *sql =
        "SELECT m.id, m.model, p.base_url, p.api_key_env, p.endpoint_type, m.context_window"
        " FROM models m JOIN providers p ON m.provider_name = p.name"
        " WHERE m.status != 'disabled'"
        " AND (m.degraded_until IS NULL OR m.degraded_until < unixepoch())"
        " ORDER BY m.priority";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return 0;
    int n = 0;
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        ModelCandidate *c = &out[n];
        const char *v;
        v = (const char *)sqlite3_column_text(s, 0); snprintf(c->id, sizeof(c->id), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 1); snprintf(c->model, sizeof(c->model), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 2); snprintf(c->base_url, sizeof(c->base_url), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 3); snprintf(c->api_key_env, sizeof(c->api_key_env), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 4);
        c->endpoint_type = (v && strcmp(v, "gemini") == 0) ? ENDPOINT_GEMINI : ENDPOINT_OPENAI;
        c->context_window = sqlite3_column_int(s, 5);
        if (c->context_window <= 0) c->context_window = 128000;
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

/* ── Stats + degradation ───────────────────────────────────────── */

static void model_stat_error(sqlite3 *db, const char *model_id, int status) {
    const char *col = (status == 429) ? "error_count_429" : "error_count_5xx";
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE models SET total_requests=total_requests+1, %s=%s+1,"
        " last_error_at=unixepoch() WHERE id=?", col, col);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);

    /* Check degradation threshold */
    const char *degrade_sql =
        "UPDATE models SET status='degraded',"
        " degraded_until=unixepoch()+(SELECT CAST(COALESCE(value,'300') AS INTEGER) FROM config WHERE key='health_cooldown_sec')"
        " WHERE id=?1 AND status='healthy'"
        " AND (CASE WHEN ?2=429"
        "   THEN error_count_429 >= (SELECT CAST(COALESCE(value,'10') AS INTEGER) FROM config WHERE key='health_429_threshold')"
        "   ELSE error_count_5xx >= (SELECT CAST(COALESCE(value,'3') AS INTEGER) FROM config WHERE key='health_5xx_threshold')"
        " END)"
        " AND last_error_at >= unixepoch()-(SELECT CAST(COALESCE(value,'300') AS INTEGER) FROM config WHERE key='health_window_sec');";
    sqlite3_stmt *ds;
    if (sqlite3_prepare_v2(db, degrade_sql, -1, &ds, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ds, 1, model_id, -1, SQLITE_STATIC);
        sqlite3_bind_int(ds, 2, status);
        sqlite3_step(ds); sqlite3_finalize(ds);
    }
}

static void model_stat_success(sqlite3 *db, const char *model_id,
                               int tokens_in, int tokens_out, int64_t cost_nano) {
    const char *sql = "UPDATE models SET total_requests=total_requests+1,"
        " total_tokens_in=total_tokens_in+?, total_tokens_out=total_tokens_out+?,"
        " total_cost_nano=total_cost_nano+?, last_success_at=unixepoch(),"
        " error_count_5xx=0, error_count_429=0 WHERE id=?";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_int(s, 1, tokens_in);
    sqlite3_bind_int(s, 2, tokens_out);
    sqlite3_bind_int64(s, 3, cost_nano);
    sqlite3_bind_text(s, 4, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);
}

/* ── llm_req: single LLM call with DB-driven routing ──────────── */

int llm_req(sqlite3 *db, CURL *curl, int64_t session_id, int recall) {
    Config *cfg = config_load(db);
    if (!cfg) { fprintf(stderr, "llm_req: config load failed\n"); return -1; }

    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    if (!a) { config_free(cfg); return -1; }

    char *agent_name_alloc = session_get_agent_name(db, session_id);
    const char *agent_name = agent_name_alloc ? agent_name_alloc : "default";

    /* Agent setup (for hooks + tool dispatch — schemas now come from DB) */
    AgentSetup setup;
    agent_setup_init(&setup, db, session_id, cfg, agent_name, NULL, 0, AGENT_SETUP_CLI);

    /* Estimate tool overhead for context budget */
    int tool_overhead = 0;
    {
        sqlite3_stmt *ts;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(SUM(length(name)+length(description)+length(parameters_json)),0)/4"
                " FROM tools WHERE enabled=1 AND (agent_name IS NULL OR agent_name=?)",
                -1, &ts, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ts, 1, agent_name, -1, SQLITE_STATIC);
            if (sqlite3_step(ts) == SQLITE_ROW) tool_overhead = sqlite3_column_int(ts, 0);
            sqlite3_finalize(ts);
        }
    }

    /* Generate system prompt (like tools — not stored as entry) */
    char *system_prompt = agent_build_system_prompt(db, agent_name, session_id, "agents", cfg);
    if (system_prompt)
        tool_overhead += (int)strlen(system_prompt) / 4;

    /* Context planning */
    ContextPlan plan = {0};
    if (context_plan(db, session_id, cfg, tool_overhead, &plan) != 0) {
        LOG_DEBUG_(cfg, "llm_req: context_plan failed");
        goto err;
    }
    LOG_DEBUG_(cfg, "llm_req: %d entries, cut=%d", plan.count, plan.cut);

    /* Auto-recall */
    char *recall_text = NULL;
    if (cfg->auto_recall && recall) {
        sqlite3_stmt *ust;
        if (sqlite3_prepare_v2(db,
                "SELECT content FROM entries WHERE session_id=? AND role=1 ORDER BY id DESC LIMIT 1",
                -1, &ust, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ust, 1, session_id);
            if (sqlite3_step(ust) == SQLITE_ROW) {
                const char *umsg = (const char *)sqlite3_column_text(ust, 0);
                if (umsg)
                    recall_text = context_auto_recall(db, session_id, umsg, cfg->recall_max_tokens);
            }
            sqlite3_finalize(ust);
        }
    }

    /* ── Mock mode ─────────────────────────────────────────────── */
    const char *mock_path = getenv("CCLAW_LLM_MOCK");
    if (mock_path) {
        FILE *f = fopen(mock_path, "r");
        if (!f) goto err;
        fseek(f, 0, SEEK_END); long flen = ftell(f); fseek(f, 0, SEEK_SET);
        char *mock_data = malloc((size_t)flen + 1);
        if (!mock_data) { fclose(f); goto err; }
        fread(mock_data, 1, (size_t)flen, f); mock_data[flen] = '\0';
        fclose(f);

        LlmResponse llm_resp; memset(&llm_resp, 0, sizeof(llm_resp));
        int rc;
        if (cfg->provider.endpoint_type == ENDPOINT_GEMINI)
            rc = llm_parse_response_gemini(db, a, mock_data, &llm_resp);
        else
            rc = llm_parse_response(db, a, mock_data, &llm_resp);
        free(mock_data);
        if (rc != 0) goto err;
        goto ingest_response;

    ingest_response: ;
        int64_t turn_id = db_next_turn_id(db, session_id);
        const char **tc_ids = NULL, **tc_names = NULL, **tc_args = NULL;
        int tcc = (int)llm_resp.tool_call_count;
        if (tcc > 0) {
            tc_ids = malloc((size_t)tcc * sizeof(char *));
            tc_names = malloc((size_t)tcc * sizeof(char *));
            tc_args = malloc((size_t)tcc * sizeof(char *));
            for (int i = 0; i < tcc; i++) {
                tc_ids[i] = llm_resp.tool_calls[i].id;
                tc_names[i] = llm_resp.tool_calls[i].name;
                tc_args[i] = llm_resp.tool_calls[i].arguments;
            }
        }
        TypedIngestResult ir;
        db_ingest_typed(db, session_id, turn_id, cfg->provider.model,
                        llm_resp.content, llm_resp.reasoning, llm_resp.finish_reason,
                        llm_resp.usage.prompt_tokens, llm_resp.usage.completion_tokens,
                        llm_resp.usage.cost_nano, tc_ids, tc_names, tc_args, tcc, &ir);
        free(tc_ids); free(tc_names); free(tc_args); free(ir.tc_entry_ids);
        free(recall_text); free(system_prompt); context_plan_free(&plan);
        agent_setup_destroy(&setup); arena_destroy(a); config_free(cfg); free(agent_name_alloc);
        return 0;
    }

    /* ── Routing state machine ─────────────────────────────────── */
    ModelCandidate models[MAX_MODELS];
    int nmodels = load_candidates(db, models, MAX_MODELS);

    /* Fallback: if no models in DB, use config provider directly */
    if (nmodels == 0) {
        ModelCandidate *m = &models[0];
        snprintf(m->id, sizeof(m->id), "config/%s", cfg->provider.model ? cfg->provider.model : "unknown");
        snprintf(m->model, sizeof(m->model), "%s", cfg->provider.model ? cfg->provider.model : "unknown");
        snprintf(m->base_url, sizeof(m->base_url), "%s", cfg->provider.base_url ? cfg->provider.base_url : "");
        m->api_key_env[0] = '\0'; /* key already in cfg */
        m->endpoint_type = cfg->provider.endpoint_type;
        m->context_window = cfg->provider.context_window;
        nmodels = 1;
    }

    uint32_t skip_mask = 0;
    int last_status = -1;
    int llm_ok = 0;
    LlmResponse llm_resp;
    memset(&llm_resp, 0, sizeof(llm_resp));
    const char *used_model_id = NULL;

    for (int mi = 0; mi < nmodels && !llm_ok; mi++) {
        if (skip_mask & (1u << mi)) continue;
        ModelCandidate *m = &models[mi];

        /* Build config for this model */
        Config route_cfg = *cfg;
        ProviderConfig route_prov = cfg->provider;
        route_prov.base_url = m->base_url;
        route_prov.model = m->model;
        route_prov.context_window = m->context_window;
        route_prov.endpoint_type = m->endpoint_type;
        const char *key = m->api_key_env[0] ? getenv(m->api_key_env) : cfg->provider.api_key;
        char *key_buf = key ? strdup(key) : strdup("");
        route_prov.api_key = key_buf;
        route_cfg.provider = route_prov;

        /* Build payload (zero-copy — holds stmt open) */
        LlmPayload payload;
        if (llm_build_payload(db, session_id, &route_cfg, &plan, recall_text, system_prompt, &payload) != 0) {
            free(key_buf); continue;
        }

        /* Build URL + auth */
        char *url = llm_build_url(a, &route_cfg);
        char *auth = llm_build_auth_header(a, &route_cfg);
        if (!url || !auth) { llm_payload_release(&payload); free(key_buf); continue; }

        char session_hdr[64];
        snprintf(session_hdr, sizeof(session_hdr), "x-session-id: cclaw-%lld", (long long)session_id);
        const char *headers[] = { "Content-Type: application/json", auth, session_hdr, NULL };

        /* Inner loop: 429 retry */
        for (int retry = 0; retry <= MAX_429_RETRIES; retry++) {
            HttpResponse resp = {0};
            SseCtx sse_ctx = {0};
            struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);

            HttpRequestOpts opts = {
                .url = url, .method = "POST", .headers = headers,
                .body = payload.body, .curl_handle = curl,
                .sse_cb = route_cfg.stream ? llm_sse_stdout_cb : NULL,
                .sse_data = NULL,
                .out_ctx = route_cfg.stream ? &sse_ctx : NULL,
            };
            int status = http_do(&opts, &resp);

            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            long elapsed = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            last_status = status;
            LOG_DEBUG_(cfg, "llm_req: %ldms status=%d model=%s", elapsed, status, m->model);

            if (cfg->log_level >= LOG_LEVEL_TRACE && resp.data)
                LOG_TRACE_(cfg, "RESP %s", resp.data);

            /* 429: retry with backoff */
            if (status == 429) {
                int wait = resp.retry_after > 0 ? resp.retry_after : (1 << retry);
                http_response_free(&resp); sse_ctx_free(&sse_ctx);
                model_stat_error(db, m->id, 429);
                if (retry < MAX_429_RETRIES) { sleep((unsigned)wait); continue; }
                break;
            }
            /* 5xx: retry with backoff, degrade after exhaustion */
            if (status >= 500 && status < 600) {
                int wait = resp.retry_after > 0 ? resp.retry_after : (1 << retry);
                http_response_free(&resp); sse_ctx_free(&sse_ctx);
                model_stat_error(db, m->id, status);
                if (retry < MAX_429_RETRIES) { sleep((unsigned)wait); continue; }
                break;
            }
            /* Prompt-specific errors: skip this model */
            if (status == 400 && llm_is_context_overflow(resp.data)) {
                http_response_free(&resp); sse_ctx_free(&sse_ctx);
                skip_mask |= (1u << mi); break;
            }
            if (status == 401 || status == 403 || status == 404) {
                http_response_free(&resp); sse_ctx_free(&sse_ctx);
                skip_mask |= (1u << mi); break;
            }
            /* Network/timeout error */
            if (status < 0) {
                http_response_free(&resp); sse_ctx_free(&sse_ctx);
                break;
            }
            /* Non-2xx */
            if (status < 200 || status >= 300) {
                http_response_free(&resp); sse_ctx_free(&sse_ctx);
                break;
            }

            /* ── Parse response ────────────────────────────────── */
            int rc;
            if (route_cfg.stream)
                rc = llm_response_from_sse(a, &sse_ctx, &llm_resp);
            else if (route_cfg.provider.endpoint_type == ENDPOINT_GEMINI)
                rc = llm_parse_response_gemini(db, a, resp.data, &llm_resp);
            else
                rc = llm_parse_response(db, a, resp.data, &llm_resp);
            http_response_free(&resp); sse_ctx_free(&sse_ctx);

            if (rc != 0 || !llm_resp.finish_reason) break; /* parse failure */

            /* Zero-usage empty stop (provider glitch) */
            if (llm_resp.usage.total_tokens == 0 &&
                (!llm_resp.content || !llm_resp.content[0]) &&
                strcmp(llm_resp.finish_reason, "stop") == 0) {
                skip_mask |= (1u << mi); break; /* try next model */
            }

            used_model_id = m->id;
            llm_ok = 1;
            break;
        }

        llm_payload_release(&payload);
        free(key_buf);
    }

    free(recall_text);
    free(system_prompt); system_prompt = NULL;
    context_plan_free(&plan); memset(&plan, 0, sizeof(plan));

    if (!llm_ok) {
        int64_t turn_id = db_next_turn_id(db, session_id);
        const char *err_text =
            last_status == -2 ? "error: request timed out"
            : last_status == -1 ? "error: network error"
            : last_status == 401 || last_status == 403 ? "error: authentication failed"
            : last_status == 404 ? "error: model not available"
            : last_status == 429 ? "error: rate limited"
            : last_status >= 500 ? "error: provider server error"
            : "error: LLM request failed";
        Message err_msg = {.role = ROLE_ASSISTANT, .content = (char *)err_text,
                           .stop_reason = STOP_REASON_ERROR, .model = cfg->provider.model};
        entry_append_with_turn(db, session_id, &err_msg, turn_id);
        goto err;
    }

    /* ── Write response to DB ──────────────────────────────────── */
    model_stat_success(db, used_model_id,
                       llm_resp.usage.prompt_tokens,
                       llm_resp.usage.completion_tokens,
                       llm_resp.usage.cost_nano);

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
                             used_model_id ? used_model_id : cfg->provider.model,
                             llm_resp.content, llm_resp.reasoning, llm_resp.finish_reason,
                             llm_resp.usage.prompt_tokens, llm_resp.usage.completion_tokens,
                             llm_resp.usage.cost_nano,
                             tc_ids, tc_names, tc_args, tc_count, &ir);
    free(tc_ids); free(tc_names); free(tc_args);
    if (rc != 0) { LOG_DEBUG_(cfg, "llm_req: db_ingest_typed failed"); goto err; }
    free(ir.tc_entry_ids);

    agent_setup_destroy(&setup);
    arena_destroy(a);
    config_free(cfg);
    free(agent_name_alloc);
    return 0;

err:
    free(system_prompt);
    context_plan_free(&plan);
    agent_setup_destroy(&setup);
    arena_destroy(a);
    config_free(cfg);
    free(agent_name_alloc);
    return -1;
}

/* ── llm_proc_main: fork-mode entry point ──────────────────────── */

int llm_proc_main(int64_t session_id) {
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
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

    int recall_flag = 0;
    const char *recall_env = getenv("CCLAW_RECALL");
    if (recall_env && recall_env[0] == '1') recall_flag = 1;

    int rc = llm_req(db, NULL, session_id, recall_flag);
    if (rc != 0) { db_close(db); return LLM_EXIT_ERROR; }

    int tc = turn_complete(db, session_id);
    db_close(db);
    return (tc == 1) ? LLM_EXIT_TOOLCALL : LLM_EXIT_STOP;
}
