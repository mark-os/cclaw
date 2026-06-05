#define _POSIX_C_SOURCE 200809L
#include "llm_child.h"
#include "config.h"
#include "context.h"
#include "db.h"
#include "db_response.h"
#include "gemini_cache.h"
#include "http.h"
#include "llm.h"
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

/* V2: retry config */
#define MAX_RETRIES 5
#define INITIAL_BACKOFF_MS 1000
#define MAX_LLM_RETRIES 3

/* SSE streaming callback: write content tokens to stdout */
static int sse_token_cb(const char *token, size_t len, void *userdata) {
    (void)userdata;
    if (token && len > 0)
        fwrite(token, 1, len, stdout);
    fflush(stdout);
    return 0;
}

/* Build URL for chat completions endpoint */
static char *build_url(Arena *a, const Config *cfg) {
    const char *base = cfg->provider.base_url;
    size_t blen = strlen(base);
    if (blen > 0 && base[blen - 1] == '/') blen--;

    if (cfg->provider.endpoint_type == ENDPOINT_GEMINI) {
        const char *model = cfg->provider.model ? cfg->provider.model : "gemini-2.5-flash";
        size_t mlen = strlen(model);
        const char *action = cfg->stream ? "streamGenerateContent?alt=sse" : "generateContent";
        size_t alen = strlen(action);
        size_t need = blen + 8 + mlen + 1 + alen + 1;
        char *url = arena_alloc(a, need);
        if (!url) return NULL;
        snprintf(url, need, "%.*s/models/%s:%s", (int)blen, base, model, action);
        return url;
    }

    const char *path = "/chat/completions";
    size_t plen = strlen(path);
    char *url = arena_alloc(a, blen + plen + 1);
    if (!url) return NULL;
    memcpy(url, base, blen);
    memcpy(url + blen, path, plen + 1);
    return url;
}

/* Call LLM with retry on 429/5xx */
static int llm_call_with_retry(const char *url, const char **headers,
                               RequestStreamer *rs, HttpResponse *resp,
                               const Config *cfg) {
    int backoff_ms = INITIAL_BACKOFF_MS;
    int last_status = -1;

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        int status;
        if (cfg->stream)
            status = http_post_stream_sse(url, headers, rs_read_cb, rs,
                                          sse_token_cb, NULL, resp);
        else
            status = http_post_stream(url, headers, rs_read_cb, rs, resp);
        last_status = status;

        if (status == -1 || status == -2) return status;
        if (status == 401 || status == 403 || status == 404) return status;

        if (status == 429 || (status >= 500 && status < 600)) {
            int wait_sec = resp->retry_after > 0 ? resp->retry_after : (backoff_ms / 1000);
            if (wait_sec < 1) wait_sec = 1;
            LOG_DEBUG(cfg, "HTTP %d, retry %d/%d (wait %ds)",
                      status, attempt + 1, MAX_RETRIES, wait_sec);
            sleep((unsigned)wait_sec);
            http_response_free(resp);
            rs_reset(rs);
            backoff_ms *= 2;
            continue;
        }
        return status;
    }
    return last_status;
}

/* Try primary then fallback chain */
static int llm_call_with_fallback(Arena *a, const Config *cfg,
                                  sqlite3 *db, int64_t session_id,
                                  const ContextPlan *plan,
                                  const ToolSchema *tools, size_t tool_count,
                                  const char *recall_text,
                                  const char *gemini_cache_name,
                                  HttpResponse *resp) {
    /* Mock mode */
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
        return resp->data ? 200 : -1;
    }

    char *url = build_url(a, cfg);
    if (!url) return -1;

    /* V125: heap-alloc auth header */
    size_t key_len = cfg->provider.api_key ? strlen(cfg->provider.api_key) : 0;
    char *auth_hdr;
    if (cfg->provider.endpoint_type == ENDPOINT_GEMINI) {
        auth_hdr = arena_alloc(a, 16 + key_len + 1);
        if (!auth_hdr) return -1;
        sprintf(auth_hdr, "x-goog-api-key: %s", cfg->provider.api_key ? cfg->provider.api_key : "");
    } else {
        auth_hdr = arena_alloc(a, 22 + key_len + 1);
        if (!auth_hdr) return -1;
        sprintf(auth_hdr, "Authorization: Bearer %s", cfg->provider.api_key ? cfg->provider.api_key : "");
    }

    char *session_hdr = arena_alloc(a, 64);
    if (!session_hdr) return -1;
    snprintf(session_hdr, 64, "x-session-id: cclaw-%lld", (long long)session_id);
    const char *headers[] = { "Content-Type: application/json", auth_hdr, session_hdr, NULL };

    RequestStreamer rs;
    if (rs_init(&rs, db, session_id, cfg, plan, tools, tool_count) != 0)
        return -1;
    rs.recall_text = recall_text;
    rs.gemini_cache_name = gemini_cache_name;

    if (cfg->log_level >= LOG_LEVEL_TRACE) {
        RequestStreamer dbg_rs;
        rs_init(&dbg_rs, db, session_id, cfg, plan, tools, tool_count);
        dbg_rs.recall_text = recall_text;
        dbg_rs.gemini_cache_name = gemini_cache_name;
        size_t cap = 4096, len = 0;
        char *dbuf = malloc(cap);
        if (dbuf) {
            while (1) {
                if (len + 1024 > cap) { cap *= 2; dbuf = realloc(dbuf, cap); if (!dbuf) break; }
                size_t n = rs_read_cb(dbuf + len, 1, 1024, &dbg_rs);
                if (n == 0) break;
                len += n;
            }
            if (dbuf) { dbuf[len] = '\0'; LOG_TRACE(cfg, "REQ %s", dbuf); free(dbuf); }
        }
        rs_cleanup(&dbg_rs);
    }

    int status = llm_call_with_retry(url, headers, &rs, resp, cfg);
    rs_cleanup(&rs);

    if (status >= 200 && status < 300) return status;
    if (status >= 300 && status < 500 && status != 401 && status != 403 &&
        status != 404 && status != 429)
        return status;

    /* Fallback chain */
    for (size_t i = 0; i < cfg->fallback_count; i++) {
        LOG_DEBUG(cfg, "primary failed, trying fallback %zu", i);
        const ProviderConfig *fb = &cfg->fallback_providers[i];
        if (!fb->base_url || !fb->api_key || !fb->model) continue;

        size_t fblen = strlen(fb->base_url);
        if (fblen > 0 && fb->base_url[fblen - 1] == '/') fblen--;
        const char *path = "/chat/completions";
        size_t plen = strlen(path);
        char *fb_url = arena_alloc(a, fblen + plen + 1);
        if (!fb_url) continue;
        memcpy(fb_url, fb->base_url, fblen);
        memcpy(fb_url + fblen, path, plen + 1);

        Config fb_cfg = *cfg;
        fb_cfg.provider = *fb;

        RequestStreamer fb_rs;
        if (rs_init(&fb_rs, db, session_id, &fb_cfg, plan, tools, tool_count) != 0)
            continue;
        fb_rs.recall_text = recall_text;

        size_t fb_key_len = strlen(fb->api_key);
        char *fb_auth = arena_alloc(a, 22 + fb_key_len + 1);
        if (!fb_auth) { rs_cleanup(&fb_rs); continue; }
        sprintf(fb_auth, "Authorization: Bearer %s", fb->api_key);
        const char *fb_headers[] = { "Content-Type: application/json", fb_auth, session_hdr, NULL };

        http_response_free(resp);
        status = llm_call_with_retry(fb_url, fb_headers, &fb_rs, resp, cfg);
        rs_cleanup(&fb_rs);
        if (status != -1) return status;
    }
    return status;
}

/* Detect context overflow */
static int is_context_overflow(const char *body) {
    if (!body) return 0;
    return (strstr(body, "context_length_exceeded") != NULL ||
            strstr(body, "maximum context length") != NULL ||
            strstr(body, "too many tokens") != NULL ||
            strstr(body, "context window") != NULL);
}

int llm_child_main(int64_t session_id) {
    /* V34: die if parent dies */
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    shutdown_init();

    Config *cfg = config_load_from_env();
    if (!cfg) { fprintf(stderr, "llm: config load failed\n"); return LLM_EXIT_ERROR; }

    const char *db_path = getenv("CCLAW_AGENT_DB");
    if (!db_path || !db_path[0]) db_path = cfg->db_path;

    sqlite3 *db = db_open_agent(db_path);
    if (!db) { config_free(cfg); return LLM_EXIT_ERROR; }
    db_set_agent_pragmas(db);

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
        struct timespec t_start;
        clock_gettime(CLOCK_MONOTONIC, &t_start);

        int status = llm_call_with_fallback(a, call_cfg, db, session_id,
                                            &plan, tools, tool_count,
                                            recall_text, gcache, &resp);
        struct timespec t_end;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                          (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
        last_status = status;
        LOG_DEBUG(cfg, "llm: %ldms, status=%d", elapsed_ms, status);

        if (cfg->log_level >= LOG_LEVEL_TRACE && resp.data)
            LOG_TRACE(cfg, "RESP status=%d %s", status, resp.data);

        /* Context overflow → exit 1 (parent handles) */
        if (status == 400 && is_context_overflow(resp.data)) {
            LOG_DEBUG(cfg, "E5: context overflow");
            http_response_free(&resp);
            goto err;
        }

        /* Non-retryable failures */
        if (status == -2 || status == -1 || status == 401 || status == 403 ||
            status == 404 || status == 429 || (status >= 500 && status < 600)) {
            http_response_free(&resp);
            break;
        }

        if (status < 200 || status >= 300 || !resp.data) {
            http_response_free(&resp);
            continue;
        }

        /* Parse response */
        int rc;
        if (call_cfg->provider.endpoint_type == ENDPOINT_GEMINI && !call_cfg->stream)
            rc = llm_parse_response_gemini(a, resp.data, &llm_resp);
        else
            rc = llm_parse_response(a, resp.data, &llm_resp);
        http_response_free(&resp);

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

    /* Write response to DB using db_ingest_streaming */
    int64_t turn_id = db_next_turn_id(db, session_id);

    /* Get current leaf_id as parent for new entry */
    int64_t parent_id = -1;
    {
        sqlite3_stmt *lf;
        if (sqlite3_prepare_v2(db, "SELECT leaf_id FROM sessions WHERE id=?;",
                               -1, &lf, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(lf, 1, session_id);
            if (sqlite3_step(lf) == SQLITE_ROW)
                parent_id = sqlite3_column_int64(lf, 0);
            sqlite3_finalize(lf);
        }
    }

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

    IngestResult ir;
    int rc = db_ingest_streaming(db, session_id, parent_id, turn_id,
                                 cfg->provider.model,
                                 llm_resp.content, llm_resp.finish_reason,
                                 llm_resp.usage.prompt_tokens,
                                 llm_resp.usage.completion_tokens,
                                 llm_resp.usage.cost_nano,
                                 tc_ids, tc_names, tc_args, tc_count, &ir);
    free(tc_ids);
    free(tc_names);
    free(tc_args);

    if (rc != 0) {
        LOG_DEBUG(cfg, "llm: db_ingest_streaming failed");
        goto err;
    }

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
