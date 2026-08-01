#define _POSIX_C_SOURCE 200809L
#include "llm_proc.h"
#include "agent_config.h"
#include "config.h"
#include "config_registry.h"
#include "context.h"
#include "db.h"
#include "db_response.h"

#include "hook_dispatch.h"
#include "http.h"
#include "llm.h"
#include "llm_payload.h"
#include "channel_api.h"
#include "llm_transport.h"
#include "log.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAX_MODELS 16
#define MAX_RETRIES 3          /* transient failures: attempts per model */
#define MAX_TIMEOUT_RETRIES 1  /* timeouts burn minutes per attempt — one retry */
/* Generous ceiling on any provider response body — a real completion is a few
 * MB at most; this only bounds daemon OOM from a malicious/buggy provider
 * (web_fetch/js_http already cap their own). */
#define LLM_RESP_MAX (32u * 1024 * 1024)



/* ── Model candidates (loaded from DB; struct in llm_proc.h) ───── */

/* True if this provider's credential can actually be resolved: none needed
 * (blank api_key_env — a local endpoint), present in the environment, or
 * present in the encrypted secrets table.
 *
 * Routing must not offer a model whose key is missing. Without this, a fresh
 * install holding only OPENAI_API_KEY still routes turn 1 to the priority-0
 * seeded openrouter row, sends an empty "Authorization: Bearer", takes a 401,
 * and degrades a healthy model for 300s. It is also what finally connects
 * config_load's key-availability scan to chat routing: when nothing here has a
 * key, llm_req falls through to the synthetic candidate built from
 * cfg->provider, which is the row that scan promoted.
 *
 * Existence only — no decrypt. getenv is why this can't be a SQL predicate. */
static int provider_key_available(sqlite3 *db, const char *api_key_env) {
    if (!api_key_env || !api_key_env[0]) return 1;
    const char *v = getenv(api_key_env);
    if (v && v[0]) return 1;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM secrets WHERE name=?1 AND scope='system' LIMIT 1",
            -1, &s, NULL) != SQLITE_OK)
        return 1;                    /* can't tell — don't block routing */
    sqlite3_bind_text(s, 1, api_key_env, -1, SQLITE_STATIC);
    int have = (sqlite3_step(s) == SQLITE_ROW);
    sqlite3_finalize(s);
    if (!have)
        LOG_DEBUG_("llm: skipping candidate — no key for %s", api_key_env);
    return have;
}

static int load_candidates(sqlite3 *db, const char *agent_name, ModelCandidate *out, int max) {
    if (max <= 0) return 0;
    int n = 0;

    /* Helper: load a single candidate by model id/name if healthy.
     * Returns 1 if loaded, 0 otherwise. */
    #define TRY_MODEL(model_ref) do { \
        const char *_mr = (model_ref); \
        if (_mr && _mr[0] && n < max) { \
            int dup = 0; \
            for (int _d = 0; _d < n; _d++) \
                if (strcmp(out[_d].id, _mr) == 0) { dup = 1; break; } \
            if (!dup) { \
                const char *lsql = \
                    "SELECT m.id, m.model, p.base_url, p.api_key_env, p.endpoint_type, m.context_window" \
                    " FROM models m JOIN providers p ON m.provider_name = p.name" \
                    " WHERE (m.id = ?1 OR m.model = ?1)" \
                    " AND m.status != 'disabled'" \
                    " AND (m.degraded_until IS NULL OR m.degraded_until < unixepoch())" \
                    " LIMIT 1;"; \
                sqlite3_stmt *ls; \
                if (sqlite3_prepare_v2(db, lsql, -1, &ls, NULL) == SQLITE_OK) { \
                    sqlite3_bind_text(ls, 1, _mr, -1, SQLITE_STATIC); \
                    if (sqlite3_step(ls) == SQLITE_ROW) { \
                        ModelCandidate *c = &out[n]; \
                        memset(c, 0, sizeof(*c)); \
                        const char *v; \
                        v = (const char *)sqlite3_column_text(ls, 0); snprintf(c->id, sizeof(c->id), "%s", v ? v : ""); \
                        v = (const char *)sqlite3_column_text(ls, 1); snprintf(c->model, sizeof(c->model), "%s", v ? v : ""); \
                        v = (const char *)sqlite3_column_text(ls, 2); snprintf(c->base_url, sizeof(c->base_url), "%s", v ? v : ""); \
                        v = (const char *)sqlite3_column_text(ls, 3); snprintf(c->api_key_env, sizeof(c->api_key_env), "%s", v ? v : ""); \
                        v = (const char *)sqlite3_column_text(ls, 4); \
                        c->endpoint_type = (v && strcmp(v, "gemini") == 0) ? ENDPOINT_GEMINI : ENDPOINT_OPENAI; \
                        c->context_window = sqlite3_column_int(ls, 5); \
                        if (c->context_window <= 0) c->context_window = 128000; \
                        if (provider_key_available(db, c->api_key_env)) n++; \
                    } \
                    sqlite3_finalize(ls); \
                } \
            } \
        } \
    } while (0)

    /* 1. Agent's primary and secondary models */
    char agent_primary[128] = "", agent_secondary[128] = "";
    if (agent_name && agent_name[0]) {
        sqlite3_stmt *as;
        if (sqlite3_prepare_v2(db,
                "SELECT primary_model, secondary_model FROM agents WHERE name=?;",
                -1, &as, NULL) == SQLITE_OK) {
            sqlite3_bind_text(as, 1, agent_name, -1, SQLITE_STATIC);
            if (sqlite3_step(as) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(as, 0);
                if (v) snprintf(agent_primary, sizeof(agent_primary), "%s", v);
                v = (const char *)sqlite3_column_text(as, 1);
                if (v) snprintf(agent_secondary, sizeof(agent_secondary), "%s", v);
            }
            sqlite3_finalize(as);
        }
    }
    TRY_MODEL(agent_primary);
    TRY_MODEL(agent_secondary);

    /* 2. System default primary/secondary from config */
    char *cfg_primary = config_get(db, "default_primary_model");
    char *cfg_secondary = config_get(db, "default_secondary_model");
    TRY_MODEL(cfg_primary);
    TRY_MODEL(cfg_secondary);
    free(cfg_primary);
    free(cfg_secondary);

    /* 3. Remaining healthy models by priority, skipping already-added */
    const char *sql =
        "SELECT m.id, m.model, p.base_url, p.api_key_env, p.endpoint_type, m.context_window"
        " FROM models m JOIN providers p ON m.provider_name = p.name"
        " WHERE m.status != 'disabled'"
        " AND (m.degraded_until IS NULL OR m.degraded_until < unixepoch())"
        " ORDER BY m.priority";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        while (n < max && sqlite3_step(s) == SQLITE_ROW) {
            const char *rid = (const char *)sqlite3_column_text(s, 0);
            if (!rid) continue;
            /* skip duplicates */
            int dup = 0;
            for (int d = 0; d < n; d++)
                if (strcmp(out[d].id, rid) == 0) { dup = 1; break; }
            if (dup) continue;

            ModelCandidate *c = &out[n];
            memset(c, 0, sizeof(*c));
            const char *v;
            snprintf(c->id, sizeof(c->id), "%s", rid);
            v = (const char *)sqlite3_column_text(s, 1); snprintf(c->model, sizeof(c->model), "%s", v ? v : "");
            v = (const char *)sqlite3_column_text(s, 2); snprintf(c->base_url, sizeof(c->base_url), "%s", v ? v : "");
            v = (const char *)sqlite3_column_text(s, 3); snprintf(c->api_key_env, sizeof(c->api_key_env), "%s", v ? v : "");
            v = (const char *)sqlite3_column_text(s, 4);
            c->endpoint_type = (v && strcmp(v, "gemini") == 0) ? ENDPOINT_GEMINI : ENDPOINT_OPENAI;
            c->context_window = sqlite3_column_int(s, 5);
            if (c->context_window <= 0) c->context_window = 128000;
            if (provider_key_available(db, c->api_key_env)) n++;
        }
        sqlite3_finalize(s);
    }

    #undef TRY_MODEL
    return n;
}

int model_pick_by_capability(sqlite3 *db, const char *cap, ModelCandidate *out, int max) {
    if (max <= 0) return 0;
    int n = 0;
    const char *sql =
        "SELECT m.id, m.model, p.base_url, p.api_key_env, p.endpoint_type, m.context_window"
        " FROM models m JOIN providers p ON m.provider_name = p.name"
        " WHERE m.status != 'disabled'"
        " AND (m.degraded_until IS NULL OR m.degraded_until < unixepoch())"
        " AND m.capabilities IS NOT NULL AND json_valid(m.capabilities)"
        " AND EXISTS (SELECT 1 FROM json_each(m.capabilities) WHERE value = ?1)"
        " ORDER BY m.priority";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(s, 1, cap, -1, SQLITE_STATIC);
    while (n < max && sqlite3_step(s) == SQLITE_ROW) {
        ModelCandidate *c = &out[n];
        memset(c, 0, sizeof(*c));
        const char *v;
        v = (const char *)sqlite3_column_text(s, 0); snprintf(c->id, sizeof(c->id), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 1); snprintf(c->model, sizeof(c->model), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 2); snprintf(c->base_url, sizeof(c->base_url), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 3); snprintf(c->api_key_env, sizeof(c->api_key_env), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 4);
        c->endpoint_type = (v && strcmp(v, "gemini") == 0) ? ENDPOINT_GEMINI : ENDPOINT_OPENAI;
        c->context_window = sqlite3_column_int(s, 5);
        if (c->context_window <= 0) c->context_window = 128000;
        /* Deliberately NOT key-filtered like chat routing: this picker's
         * contract is health (degraded/disabled/capability), and a media job
         * has its own attempt budget and failure entry. Applying
         * provider_key_available here would turn a 401 carrying provider
         * detail into a bare "no capable model" — a separate call to make
         * alongside the media_jobs failure path, not a side effect of the
         * chat-routing fix. */
        n++;
    }
    sqlite3_finalize(s);
    return n;
}

/* ── Stats + degradation ───────────────────────────────────────── */

/* Tell the operator (via the session's channel chat) that routing changed.
 * Best-effort — a CLI session or notify failure never blocks the turn. */
static void notify_degraded(sqlite3 *db, const char *db_path, int64_t session_id,
                            const char *model_id, int status, const char *why) {
    char detail[24];
    if (status > 0) snprintf(detail, sizeof(detail), "http %d", status);
    else snprintf(detail, sizeof(detail), "no response");
    char text[256];
    snprintf(text, sizeof(text),
             "⚠ model %s degraded (%s, %s) — requests fall back to the "
             "next candidate until it recovers. /model shows the routing order.",
             model_id, why, detail);
    channel_notify_session(db, db_path, session_id, text);
}

static void model_stat_error(sqlite3 *db, const char *db_path, int64_t session_id,
                             const char *model_id, int status) {
    const char *col = (status == 429) ? "error_count_429" : "error_count_5xx";
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE models SET total_requests=total_requests+1, %s=%s+1,"
        " last_error_at=unixepoch() WHERE id=?", col, col);
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);

    /* Check degradation threshold. status='healthy' guard makes this fire on
     * the transition only (model_stat_success restores 'healthy'). */
    const char *degrade_sql =
        "UPDATE models SET status='degraded',"
        " degraded_until=unixepoch()+(SELECT CAST(COALESCE(value, default_value, '300') AS INTEGER) FROM config WHERE key='health_cooldown_sec')"
        " WHERE id=?1 AND status='healthy'"
        " AND (CASE WHEN ?2=429"
        "   THEN error_count_429 >= (SELECT CAST(COALESCE(value, default_value, '10') AS INTEGER) FROM config WHERE key='health_429_threshold')"
        "   ELSE error_count_5xx >= (SELECT CAST(COALESCE(value, default_value, '3') AS INTEGER) FROM config WHERE key='health_5xx_threshold')"
        " END)"
        " AND last_error_at >= unixepoch()-(SELECT CAST(COALESCE(value, default_value, '300') AS INTEGER) FROM config WHERE key='health_window_sec');";
    sqlite3_stmt *ds;
    if (sqlite3_prepare_v2(db, degrade_sql, -1, &ds, NULL) == SQLITE_OK) {
        sqlite3_bind_text(ds, 1, model_id, -1, SQLITE_STATIC);
        sqlite3_bind_int(ds, 2, status);
        sqlite3_step(ds);
        if (sqlite3_changes(db) > 0) {
            LOG_INFO_("model degraded model=%s status=%d", model_id, status);
            notify_degraded(db, db_path, session_id, model_id, status,
                            status == 429 ? "rate limited" : "repeated errors");
        }
        sqlite3_finalize(ds);
    }
}

/* Auth/not-found failures (401/403/404) don't self-heal within a turn:
 * sideline the model for a cooldown so it isn't retried every request, and
 * tell the operator once (on the healthy→degraded transition). The cooldown
 * still re-probes periodically in case the key/model was fixed. */
static void model_degrade_unavailable(sqlite3 *db, const char *db_path,
                                      int64_t session_id, const char *model_id,
                                      int status) {
    const char *sql =
        "UPDATE models SET status='degraded', last_error_at=unixepoch(),"
        " degraded_until=unixepoch()+(SELECT CAST(COALESCE(value, default_value, '300') AS INTEGER) FROM config WHERE key='health_cooldown_sec')"
        " WHERE id=?1;";
    /* Transition check first — the UPDATE below always extends the cooldown. */
    int was_healthy = 0;
    sqlite3_stmt *cs;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM models WHERE id=?1 AND status='healthy'",
                           -1, &cs, NULL) == SQLITE_OK) {
        sqlite3_bind_text(cs, 1, model_id, -1, SQLITE_STATIC);
        was_healthy = sqlite3_step(cs) == SQLITE_ROW;
        sqlite3_finalize(cs);
    }
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    if (changed > 0 && was_healthy) {
        LOG_INFO_("model degraded model=%s status=%d reason=unavailable", model_id, status);
        notify_degraded(db, db_path, session_id, model_id, status,
                        status == 404 ? "model not found" : "auth failed — check its key");
    }
}

static void model_stat_success(sqlite3 *db, const char *model_id,
                               int tokens_in, int tokens_out, int64_t cost_nano) {
    /* Success is recovery: restore 'healthy' so the degradation guard in
     * model_stat_error can fire again on the next real outage. */
    const char *sql = "UPDATE models SET total_requests=total_requests+1,"
        " total_tokens_in=total_tokens_in+?, total_tokens_out=total_tokens_out+?,"
        " total_cost_nano=total_cost_nano+?, last_success_at=unixepoch(),"
        " error_count_5xx=0, error_count_429=0,"
        " status='healthy', degraded_until=NULL"
        " WHERE id=? AND status != 'disabled'";
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
    if (!cfg) { LOG_ERROR_("llm_req: config load failed"); return -1; }

    char *agent_name_alloc = session_get_agent_name(db, session_id);
    const char *agent_name = agent_name_alloc ? agent_name_alloc : "Assistant";

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

    /* Declared before any goto err so the err: label can safely free it,
     * even on the early context_plan-failure path where it isn't set yet. */
    char *context_text = NULL;

    /* Resolve effective context window for this agent's model */
    int effective_window = model_context_window(db, agent_name, cfg->context_window);

    /* Context planning — use a stack-local config with the resolved window */
    Config plan_cfg = *cfg;
    plan_cfg.context_window = effective_window;
    ContextPlan plan = {0};
    if (context_plan(db, session_id, &plan_cfg, tool_overhead, &plan) != 0) {
        LOG_DEBUG_("llm_req: context_plan failed");
        goto err;
    }
    LOG_DEBUG_("llm_req: %d entries, cut=%d", plan.count, plan.cut);

    /* Session context: recall plus live per-turn state (context-placement
     * memory blocks, running sub-agents, pending approvals) — never baked
     * into the system prompt. Materialized ONCE at turn start (recall != 0
     * ⟺ iteration 0) and persisted on the session row; tool-loop iterations
     * read the frozen text back verbatim. Content and position (turn
     * boundary, see llm_payload.c) thus stay byte-stable across the whole
     * turn, so every iteration extends the provider's prompt-cache prefix
     * instead of invalidating it. Live state is a turn-start snapshot by
     * design — mid-turn changes surface as tool results, not here. */
    if (recall) {
        char *recall_text = NULL;
        if (cfg->auto_recall) {
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
        context_text = session_context_text(db, session_id, recall_text);
        free(recall_text);
        session_set_turn_context(db, session_id, context_text);
    } else {
        context_text = session_get_turn_context(db, session_id);
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

        int64_t iteration_id = db_next_iteration_id(db, session_id);
        TypedIngestResult ir;
        LlmRespStatus st = db_ingest_response(db, session_id, iteration_id,
                               cfg->provider.model, cfg->provider.endpoint_type,
                               mock_data, NULL, cfg->save_reasoning, &ir);
        free(mock_data);

        if (st != LLM_RESP_OK) {
            Message err_msg = {.role = ROLE_ASSISTANT, .content = "error: malformed LLM response",
                               .stop_reason = STOP_REASON_ERROR, .model = cfg->provider.model};
            entry_append_with_iteration(db, session_id, &err_msg, 0);
        }
        free(context_text); free(system_prompt); context_plan_free(&plan);
        hook_directives_clear(db, session_id);  /* "this request only" */
        config_free(cfg); free(agent_name_alloc);
        return (st == LLM_RESP_OK) ? 0 : -1;
    }

    /* ── Routing state machine ─────────────────────────────────── */
    ModelCandidate models[MAX_MODELS];
    int nmodels = load_candidates(db, agent_name, models, MAX_MODELS);

    /* Fallback: if no models in DB, use config provider directly */
    if (nmodels == 0) {
        ModelCandidate *m = &models[0];
        snprintf(m->id, sizeof(m->id), "config/%s", cfg->provider.model ? cfg->provider.model : "unknown");
        snprintf(m->model, sizeof(m->model), "%s", cfg->provider.model ? cfg->provider.model : "unknown");
        snprintf(m->base_url, sizeof(m->base_url), "%s", cfg->provider.base_url ? cfg->provider.base_url : "");
        m->api_key_env[0] = '\0'; /* key already in cfg */
        m->use_cfg_key = 1;       /* ...and this is the one row allowed to use it */
        m->endpoint_type = cfg->provider.endpoint_type;
        m->context_window = cfg->context_window;
        nmodels = 1;
    }

    int llm_ok = 0;
    int had_dberr = 0;  /* set if any model hit LLM_RESP_DBERR (our-side DB failure) */
    /* Most recent failure, phrased for the error entry. Every failure path in
     * the loop below sets this; the init value only survives if no attempt was
     * ever made (payload/url build failed for every candidate). */
    const char *fail_text = "error: LLM request failed";

    /* One iteration id for this LLM request: every raw response archived below
     * (the failed attempts and the final ingest) shares it. db_next_iteration_id
     * is a pure read, so it stays stable until the success path inserts entries.
     * The *turn* this iteration belongs to is carried by the entries themselves
     * (entries.turn_id, filled from the parent by entries_turn_ai). */
    int64_t iteration_id = db_next_iteration_id(db, session_id);

    for (int mi = 0; mi < nmodels && !llm_ok && !had_dberr; mi++) {
        ModelCandidate *m = &models[mi];

        /* Build config for this model */
        Config route_cfg = *cfg;
        ProviderConfig route_prov = cfg->provider;
        route_prov.base_url = m->base_url;
        route_prov.model = m->model;
        route_prov.endpoint_type = m->endpoint_type;
        /* env → encrypted kv → cfg. Re-reading kv here (not mutating cfg,
         * which worker threads share) picks up keys set after startup.
         * cfg->provider.api_key is only reachable for the synthetic candidate
         * (use_cfg_key): a providers row with a blank api_key_env — the schema
         * default, and what the dashboard's add-provider stores — must not
         * borrow whichever key the availability scan happened to select, or we
         * attach one provider's credential to another's base_url. */
        const char *key = m->api_key_env[0] ? getenv(m->api_key_env)
                        : m->use_cfg_key    ? cfg->provider.api_key
                        : NULL;
        char *key_buf = (key && key[0]) ? strdup(key)
                      : m->api_key_env[0] ? db_secret_get_system(db, m->api_key_env)
                      : NULL;
        if (!key_buf) {
            if (!m->api_key_env[0] && !m->use_cfg_key)
                LOG_WARN_("llm: model %s has no api_key_env — sending unauthenticated "
                          "request to %s", m->id, m->base_url);
            key_buf = strdup("");
        }
        route_prov.api_key = key_buf;
        route_cfg.provider = route_prov;

        /* Build payload (zero-copy — holds stmt open) */
        LlmPayload payload;
        if (llm_build_payload(db, session_id, &route_cfg, &plan, context_text, system_prompt, &payload) != 0) {
            free(key_buf); continue;
        }

        /* Stash request body on the heap: payload.body is only valid while the
         * stmt is open, but we must release the stmt (drop TRANS_READ) before any
         * DB write, and the body has to outlive that release to survive 429/5xx
         * resends in the retry loop below. */
        char *req_body = payload.body ? strdup(payload.body) : NULL;
        llm_payload_release(&payload);

        /* Build URL + auth */
        char *url = llm_build_url(&route_cfg);
        char *auth = llm_build_auth_header(&route_cfg);
        if (!url || !auth) { free(url); free(auth); free(req_body); free(key_buf); continue; }

        char session_hdr[64];
        snprintf(session_hdr, sizeof(session_hdr), "x-session-id: cclaw-%lld", (long long)session_id);
        const char *headers[] = { "Content-Type: application/json", auth, session_hdr, NULL };

        /* Inner loop: same-model retry. The whole failure policy lives here:
         *
         *   transient → backoff retry this model, then the next candidate:
         *     429 / 5xx              (Retry-After honored)
         *     network error          (-1)
         *     timeout                (-2 — single retry, attempts cost minutes)
         *     2xx empty body         (gateway hiccup)
         *     2xx empty completion   (zero-usage "stop" with no content)
         *     2xx malformed body
         *   permanent for this model → next candidate immediately:
         *     401/403/404            (degraded with cooldown)
         *     400 context overflow   (prompt-specific, no degrade)
         *     other 4xx              (request rejected — resending can't help)
         *   our-side fatal → abort the turn, no further candidates:
         *     DB error during ingest (paying another provider won't fix our DB)
         */
        for (int retry = 0; retry <= MAX_RETRIES; retry++) {
            HttpResponse resp = {0};
            struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0);

            HttpRequestOpts opts = {
                .url = url, .method = "POST", .headers = headers,
                .body = req_body, .curl_handle = curl,
                .max_response_bytes = LLM_RESP_MAX,
            };
            int status = http_do(&opts, &resp);

            struct timespec t1; clock_gettime(CLOCK_MONOTONIC, &t1);
            long elapsed = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
            LOG_DEBUG_("llm_req: %ldms status=%d model=%s", elapsed, status, m->model);
            LOG_DEBUG_("llm_req: ttfb=%.0fms tls=%.0fms bytes=%zu reuse=%d",
                       resp.ttfb * 1000.0, resp.tls_time * 1000.0,
                       resp.len, resp.conn_reused);

            if (cfg->log_level >= LOG_LEVEL_TRACE && resp.data)
                LOG_TRACE_("RESP %s", resp.data);

            /* Archive every non-2xx / network response (2xx bodies are archived
             * by db_ingest_response below). */
            if (status < 200 || status >= 300) {
                char lbl[24];
                if (status == -2) snprintf(lbl, sizeof(lbl), "timeout");
                else if (status < 0) snprintf(lbl, sizeof(lbl), "network_error");
                else snprintf(lbl, sizeof(lbl), "http_%d", status);
                db_archive_response(db, session_id, iteration_id, m->id, lbl,
                                    resp.data ? resp.data : resp.err_detail,
                                    req_body);
            }

            /* 429 / 5xx: transient — backoff retry */
            if (status == 429 || (status >= 500 && status < 600)) {
                int wait = resp.retry_after > 0 ? resp.retry_after : (1 << retry);
                fail_text = status == 429 ? "error: rate limited"
                                          : "error: provider server error";
                http_response_free(&resp);
                model_stat_error(db, cfg->db_path, session_id, m->id, status);
                if (retry < MAX_RETRIES) { sleep((unsigned)wait); continue; }
                break;
            }
            /* Prompt-specific errors: skip this model */
            if (status == 400 && llm_is_context_overflow(resp.data)) {
                LOG_DEBUG_("llm_req model_skip model=%s reason=context_overflow", m->model);
                fail_text = "error: prompt too large for the model's context window";
                http_response_free(&resp);
                break;
            }
            /* Other 400: persistent rejection (bad payload shape, encoding
             * issues) — degrade so we don't retry every turn. */
            if (status == 400) {
                LOG_DEBUG_("llm_req model_skip model=%s reason=bad_request", m->model);
                fail_text = "error: provider rejected the request";
                http_response_free(&resp);
                model_degrade_unavailable(db, cfg->db_path, session_id, m->id, status);
                break;
            }
            if (status == 401 || status == 403 || status == 404) {
                LOG_DEBUG_("llm_req model_skip model=%s reason=%s", m->model,
                           status == 404 ? "not_found" : "auth_failed");
                fail_text = status == 404 ? "error: model not available"
                                          : "error: authentication failed";
                http_response_free(&resp);
                model_degrade_unavailable(db, cfg->db_path, session_id, m->id, status);
                break;
            }
            /* Timeout: transient but each attempt burns minutes — one retry,
             * no extra backoff (the failed attempt was the wait). */
            if (status == -2) {
                fail_text = "error: request timed out";
                http_response_free(&resp);
                model_stat_error(db, cfg->db_path, session_id, m->id, status);
                if (retry < MAX_TIMEOUT_RETRIES) continue;
                break;
            }
            /* Network error: transient — backoff retry */
            if (status < 0) {
                fail_text = "error: network error";
                http_response_free(&resp);
                model_stat_error(db, cfg->db_path, session_id, m->id, status);
                if (retry < MAX_RETRIES) { sleep(1u << retry); continue; }
                break;
            }
            /* Other 4xx: the request itself was rejected — resending it to the
             * same model can't change the answer; another candidate might. */
            if (status < 200 || status >= 300) {
                fail_text = "error: provider rejected the request";
                http_response_free(&resp);
                break;
            }

            /* A 2xx with an empty body is a provider/gateway hiccup (often a slow
             * near-timeout response). db_ingest_response would bail on the NULL
             * body *before* archiving, leaving an opaque error with no forensic
             * trail. Archive it (with the request we sent) and retry. */
            if (!resp.data || !resp.data[0]) {
                LOG_INFO_("llm_req empty_body model=%s retry=%d", m->model, retry);
                db_archive_response(db, session_id, iteration_id, m->id, "empty",
                                    resp.data, req_body);
                fail_text = "error: provider returned an empty response";
                http_response_free(&resp);
                model_stat_error(db, cfg->db_path, session_id, m->id, status);
                if (retry < MAX_RETRIES) { sleep(1u << retry); continue; }
                break;
            }

            /* ── Ingest response straight to the DB ── */
            TypedIngestResult ir;
            LlmRespStatus st = db_ingest_response(db, session_id, iteration_id,
                                   m->id, route_cfg.provider.endpoint_type,
                                   resp.data, req_body, route_cfg.save_reasoning, &ir);
            http_response_free(&resp);

            if (st == LLM_RESP_OK) {
                model_stat_success(db, m->id, ir.prompt_tokens, ir.completion_tokens, ir.cost_nano);
                llm_ok = 1;
                break;
            }
            /* LLM_RESP_DBERR: our DB failed (SQLITE_BUSY) — body was valid.
             * Diagnostics already logged by db_ingest_response. */
            if (st == LLM_RESP_DBERR) { had_dberr = 1; break; }
            /* EMPTY (zero-usage stop, archived 'empty') and MALFORMED (bad
             * body, archived 'malformed') are both provider glitches wrapped
             * in a 2xx: transient, same as an empty body. */
            LOG_INFO_("llm_req %s model=%s retry=%d",
                      st == LLM_RESP_EMPTY ? "empty_completion" : "malformed_body",
                      m->model, retry);
            fail_text = st == LLM_RESP_EMPTY
                ? "error: provider returned an empty response"
                : "error: provider returned a malformed response";
            model_stat_error(db, cfg->db_path, session_id, m->id, status);
            if (retry < MAX_RETRIES) { sleep(1u << retry); continue; }
            break;
        }

        free(url); free(auth);
        free(req_body);
        free(key_buf);
    }

    free(context_text); context_text = NULL;   /* err: frees again on !llm_ok */
    free(system_prompt); system_prompt = NULL;
    context_plan_free(&plan); memset(&plan, 0, sizeof(plan));

    if (!llm_ok) {
        const char *err_text = had_dberr
            ? "error: DB contention during response ingest (SQLITE_BUSY) — check logs"
            : fail_text;
        /* Cite the archived llm_responses row so the operator can pull the exact
         * request + provider reply with `cclaw resp <id>`. */
        int64_t resp_id = 0;
        sqlite3_stmt *rs;
        if (sqlite3_prepare_v2(db,
                "SELECT MAX(id) FROM llm_responses WHERE session_id=?1 AND iteration_id=?2",
                -1, &rs, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(rs, 1, session_id);
            sqlite3_bind_int64(rs, 2, iteration_id);
            if (sqlite3_step(rs) == SQLITE_ROW && sqlite3_column_type(rs, 0) != SQLITE_NULL)
                resp_id = sqlite3_column_int64(rs, 0);
            sqlite3_finalize(rs);
        }
        /* DBERR loses a valid, paid response — log it (body is in llm_responses
         * when archiving is on; resp #0 means it isn't, so this line is the only
         * trail). Metadata only: the body can be large and may carry secrets. */
        if (had_dberr)
            LOG_ERROR_("llm_req: response ingest failed (DB contention) "
                       "session=%lld turn=%lld resp=#%lld — response discarded",
                       (long long)session_id, (long long)iteration_id, (long long)resp_id);
        char err_buf[160];
        if (resp_id > 0)
            snprintf(err_buf, sizeof(err_buf), "%s [resp #%lld]", err_text, (long long)resp_id);
        else
            snprintf(err_buf, sizeof(err_buf), "%s", err_text);
        Message err_msg = {.role = ROLE_ASSISTANT, .content = err_buf,
                           .stop_reason = STOP_REASON_ERROR, .model = cfg->provider.model};
        entry_append_with_iteration(db, session_id, &err_msg, 0);
        goto err;
    }

    hook_directives_clear(db, session_id);  /* injects/suppress were this call's */
    config_free(cfg);
    free(agent_name_alloc);
    return 0;

err:
    free(context_text);
    free(system_prompt);
    context_plan_free(&plan);
    hook_directives_clear(db, session_id);
    config_free(cfg);
    free(agent_name_alloc);
    return -1;
}

/* ── Compaction: LLM-driven session summarization ─────────────── */

/* Pull the assistant text out of a response body (OpenAI or Gemini).
 * Returns a malloc'd string the caller frees, or NULL if absent. */
static char *extract_content(sqlite3 *db, EndpointType ep, const char *body) {
    if (!body) return NULL;
    const char *sql = (ep == ENDPOINT_GEMINI)
        ? "SELECT group_concat(json_extract(value,'$.text'), char(10))"
          " FROM json_each(?1,'$.candidates[0].content.parts')"
          " WHERE json_extract(value,'$.text') IS NOT NULL"
        : "SELECT json_extract(?1,'$.choices[0].message.content')";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(s, 1, body, -1, SQLITE_STATIC);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) r = strdup(t);
    }
    sqlite3_finalize(s);
    return r;
}

int llm_compaction(sqlite3 *db, CURL *curl, int64_t session_id, const char *agent_name) {
    Config *cfg = config_load(db);
    if (!cfg || !cfg->compaction) return -1;

    /* Resolve effective context window for this agent's model */
    int effective_window = model_context_window(db, agent_name, cfg->context_window);

    /* Re-check the trigger: a crash-recovered or queued-up job may run after
     * the branch was already compacted */
    if (!session_needs_compaction(db, session_id, cfg)) {
        config_free(cfg);
        return 0;
    }

    /* Compute target — keep tail entries that fit in target × context_window */
    float target_ratio = cfg->compaction_target > 0 ? cfg->compaction_target : 0.3f;
    int target_tokens = (int)(target_ratio * (float)effective_window);
    if (target_tokens <= 0) target_tokens = 4000;

    /* Plan branch — use resolved window */
    Config plan_cfg = *cfg;
    plan_cfg.context_window = effective_window;
    ContextPlan plan = {0};
    if (context_plan(db, session_id, &plan_cfg, 0, &plan) != 0) {
        config_free(cfg);
        return -1;
    }

    int keep_from = context_compaction_keep_from(&plan, target_tokens);
    if (keep_from == 0) {
        /* Nothing to compact without splitting a turn */
        context_plan_free(&plan);
        config_free(cfg);
        return 0;
    }

    int64_t last_kept_id = plan.entries[0].id;
    int64_t first_after_id = plan.entries[keep_from].id;

    /* Load content of entries to compact for LLM summarization */
    char *text = malloc(4096);
    if (!text) {
        context_plan_free(&plan);
        config_free(cfg);
        return -1;
    }
    size_t text_cap = 4096, text_len = 0;
    text[0] = '\0';

    for (int i = 1; i < keep_from && text_len < 100000; i++) {
        const char *sql = "SELECT role, content FROM entries WHERE id=? AND session_id=?;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) continue;
        sqlite3_bind_int64(stmt, 1, plan.entries[i].id);
        sqlite3_bind_int64(stmt, 2, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int role = sqlite3_column_int(stmt, 0);
            const char *content = (const char *)sqlite3_column_text(stmt, 1);
            const char *rname = role == 2 ? "assistant" : role == 1 ? "user" : role == 3 ? "tool" : "system";
            if (content) {
                size_t clen = strlen(content);
                size_t need = text_len + strlen(rname) + clen + 8;
                if (need > text_cap) {
                    while (need > text_cap) text_cap *= 2;
                    char *tmp = realloc(text, text_cap);
                    if (!tmp) { sqlite3_finalize(stmt); break; }
                    text = tmp;
                }
                text_len += (size_t)snprintf(text + text_len, text_cap - text_len,
                                             "[%s] %s\n", rname, content);
            }
        }
        sqlite3_finalize(stmt);
    }

    if (text_len == 0) {
        /* No content to summarize */
        free(text);
        context_plan_free(&plan);
        config_free(cfg);
        return 0;
    }

    const char *sys_prompt =
        "Summarize the following conversation excerpt into a structured summary. "
        "Include: goal, progress, key decisions, and next steps. "
        "Be concise (under 500 words). Output plain text only.";

    int max_tok = (cfg->provider.max_tokens > 0 && cfg->provider.max_tokens < 1024)
        ? cfg->provider.max_tokens : 1024;

    /* The body shape must follow the endpoint that llm_build_url and
     * llm_build_auth_header below are built for. config_load's
     * key-availability scan can make cfg->provider a native-Gemini row, and
     * an OpenAI-shaped body POSTed to :generateContent is a 400 — compaction
     * would then fail silently forever while the session kept growing.
     * extract_content() is already endpoint-aware; this is the request half
     * of the same split (same pattern as media_build_body). */
    int gemini = (cfg->provider.endpoint_type == ENDPOINT_GEMINI);
    /* ?1 model (OpenAI only — Gemini takes the model in the URL),
     * ?2 max tokens, ?3 system prompt, ?4 excerpt */
    const char *csql = gemini
        ? "SELECT json_object("
          "  'systemInstruction', json_object('parts',json_array(json_object('text',?3))),"
          "  'contents', json_array(json_object('role','user',"
          "    'parts', json_array(json_object('text',?4)))),"
          "  'generationConfig', json_object('maxOutputTokens',?2));"
        : "SELECT json_object('model',?1,'max_tokens',?2,'messages',json_array("
          "json_object('role','system','content',?3),"
          "json_object('role','user','content',?4)));";

    char *body = NULL;
    sqlite3_stmt *cstmt;
    if (sqlite3_prepare_v2(db, csql, -1, &cstmt, NULL) == SQLITE_OK) {
        if (!gemini)
            sqlite3_bind_text(cstmt, 1, cfg->provider.model ? cfg->provider.model : "gpt-4", -1, SQLITE_STATIC);
        sqlite3_bind_int(cstmt, 2, max_tok);
        sqlite3_bind_text(cstmt, 3, sys_prompt, -1, SQLITE_STATIC);
        sqlite3_bind_text(cstmt, 4, text, (int)text_len, SQLITE_STATIC);
        if (sqlite3_step(cstmt) == SQLITE_ROW) {
            const char *col = (const char *)sqlite3_column_text(cstmt, 0);
            body = col ? strdup(col) : NULL;
        }
        sqlite3_finalize(cstmt);
    }

    free(text);

    if (!body) {
        context_plan_free(&plan);
        config_free(cfg);
        return -1;
    }

    /* Make HTTP call */
    char *url = llm_build_url(cfg);
    char *auth = llm_build_auth_header(cfg);
    if (!url || !auth) {
        free(url); free(auth); free(body);
        context_plan_free(&plan);
        config_free(cfg);
        return -1;
    }

    const char *headers[] = { "Content-Type: application/json", auth, NULL };

    HttpResponse resp = {0};
    HttpRequestOpts opts = {
        .url = url, .method = "POST", .headers = headers,
        .body = body, .curl_handle = curl,
        .max_response_bytes = LLM_RESP_MAX,
    };
    int status = http_do(&opts, &resp);
    free(body); free(url); free(auth);

    if (status < 200 || status >= 300) {
        /* Skip compaction. Logged, not silent: a failure here leaves the
         * session growing unbounded, and the only symptom is an eventual
         * context overflow many turns later. */
        LOG_WARN_("compaction: LLM call failed status=%d session=%lld model=%s",
                  status, (long long)session_id,
                  cfg->provider.model ? cfg->provider.model : "?");
        http_response_free(&resp);
        context_plan_free(&plan);
        config_free(cfg);
        return -1;
    }

    /* Extract the summary text from the response body */
    char *summary = extract_content(db, cfg->provider.endpoint_type, resp.data);
    http_response_free(&resp);

    if (!summary) {
        /* Parse failure — skip */
        context_plan_free(&plan);
        config_free(cfg);
        return -1;
    }

    /* Call entry_compact to insert summary and reparent */
    int64_t compact_id = entry_compact(db, session_id, last_kept_id, first_after_id, summary);
    free(summary);

    context_plan_free(&plan);
    config_free(cfg);

    return (compact_id > 0) ? 0 : -1;
}


/* ── Media preprocessing: capability-routed transcription/description ─
 *
 * A media_jobs row holds the channel-emitted message JSON with media
 * {kind, mime, path}: the payload bytes live in the channel's media spool
 * file, never in the DB or the LLM context. One capability-matched model
 * turns them into text; only that text enters the session, and the spool
 * file is deleted when the job resolves. */

#define TRANSCRIBE_MAX_ATTEMPTS 3
#define TRANSCRIBE_RETRIES 1

static const char *TRANSCRIBE_PROMPT =
    "Transcribe this audio message verbatim, in its original language. "
    "Output only the transcript text, nothing else.";
static const char *DESCRIBE_PROMPT =
    "Describe this image for someone who cannot see it: subject, any visible "
    "text verbatim, and relevant detail. Output only the description.";

typedef struct {
    char kind[16];     /* "audio" (default) or "image" */
    char mime[64];
    char path[1024];   /* media spool file; empty = malformed job */
    /* derived from kind: */
    const char *cap, *prompt, *label, *fail_text;
} MediaJobInfo;

static int media_job_info(sqlite3 *db, int64_t job_id, MediaJobInfo *mi) {
    memset(mi, 0, sizeof(*mi));
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(json_extract(payload,'$.media.kind'),'audio'),"
            "       COALESCE(json_extract(payload,'$.media.mime'),''),"
            "       COALESCE(json_extract(payload,'$.media.path'),'')"
            " FROM media_jobs WHERE id=?1", -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(s, 1, job_id);
    int found = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        found = 1;
        const char *v = (const char *)sqlite3_column_text(s, 0);
        snprintf(mi->kind, sizeof(mi->kind), "%s", v ? v : "audio");
        v = (const char *)sqlite3_column_text(s, 1);
        snprintf(mi->mime, sizeof(mi->mime), "%s", v ? v : "");
        v = (const char *)sqlite3_column_text(s, 2);
        snprintf(mi->path, sizeof(mi->path), "%s", v ? v : "");
    }
    sqlite3_finalize(s);
    if (!found) return -1;
    if (strcmp(mi->kind, "image") == 0) {
        mi->cap = "image";
        mi->prompt = DESCRIBE_PROMPT;
        mi->label = "[image]";
        mi->fail_text = "[image received but description failed]";
        if (!mi->mime[0]) snprintf(mi->mime, sizeof(mi->mime), "image/jpeg");
    } else {
        mi->cap = "audio";
        mi->prompt = TRANSCRIBE_PROMPT;
        mi->label = "[voice message]";
        mi->fail_text = "[voice message received but transcription failed]";
        if (!mi->mime[0]) snprintf(mi->mime, sizeof(mi->mime), "audio/ogg");
    }
    return 0;
}

/* OpenAI input_audio wants a format token, not a mime type. */
static const char *audio_format_from_mime(const char *mime) {
    if (strstr(mime, "mpeg") || strstr(mime, "mp3")) return "mp3";
    if (strstr(mime, "mp4") || strstr(mime, "m4a")) return "m4a";
    if (strstr(mime, "wav")) return "wav";
    if (strstr(mime, "webm")) return "webm";
    if (strstr(mime, "flac")) return "flac";
    return "ogg";
}

char *transcribe_build_body(sqlite3 *db, EndpointType ep, const char *model,
                            int64_t job_id) {
    MediaJobInfo mi;
    if (media_job_info(db, job_id, &mi) != 0 || !mi.path[0]) return NULL;

    /* The spool file is read and encoded here, at request-build time — the
     * only point in the daemon where the payload is materialized. */
    size_t len = 0;
    char *raw = util_read_file(mi.path, &len);
    if (!raw) {
        LOG_WARN_("media: cannot read spool file job=%lld path=%s",
                  (long long)job_id, mi.path);
        return NULL;
    }
    char *b64 = base64_encode((const unsigned char *)raw, len);
    free(raw);
    if (!b64) return NULL;

    int is_image = (strcmp(mi.kind, "image") == 0);
    /* ?1 prompt, ?2 mime (gemini/image) or format (openai audio), ?3 data,
     * ?4 model */
    const char *sql;
    if (ep == ENDPOINT_GEMINI)
        sql = "SELECT json_object('contents', json_array(json_object("
              "  'role','user','parts', json_array("
              "    json_object('text', ?1),"
              "    json_object('inlineData', json_object("
              "      'mimeType', ?2, 'data', ?3))))))";
    else if (is_image)
        sql = "SELECT json_object('model', ?4, 'messages', json_array(json_object("
              "  'role','user','content', json_array("
              "    json_object('type','text','text', ?1),"
              "    json_object('type','image_url','image_url', json_object("
              "      'url', 'data:' || ?2 || ';base64,' || ?3))))))";
    else
        sql = "SELECT json_object('model', ?4, 'messages', json_array(json_object("
              "  'role','user','content', json_array("
              "    json_object('type','text','text', ?1),"
              "    json_object('type','input_audio','input_audio', json_object("
              "      'data', ?3, 'format', ?2))))))";

    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) { free(b64); return NULL; }
    sqlite3_bind_text(s, 1, mi.prompt, -1, SQLITE_STATIC);
    if (ep == ENDPOINT_GEMINI || is_image)
        sqlite3_bind_text(s, 2, mi.mime, -1, SQLITE_TRANSIENT);
    else
        sqlite3_bind_text(s, 2, audio_format_from_mime(mi.mime), -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, b64, -1, SQLITE_STATIC);
    if (ep != ENDPOINT_GEMINI)
        sqlite3_bind_text(s, 4, model ? model : "", -1, SQLITE_STATIC);
    char *body = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) body = strdup(t);
    }
    sqlite3_finalize(s);
    free(b64);
    return body;
}

/* Resolve the job: write `text` (with the original caption prepended, if any)
 * as a normal text-only inbox message for the job's session, then delete the
 * job row. Returns 0 on success; on inbox failure the row is kept so a
 * daemon-restart resubmit can retry. */
static int transcribe_resolve(sqlite3 *db, int64_t job_id, const char *text) {
    const char *sql =
        "SELECT session_id, source, json_object("
        "  'chat_id', json_extract(payload,'$.chat_id'),"
        "  'from', COALESCE(json_extract(payload,'$.from'),''),"
        "  'text', CASE WHEN COALESCE(json_extract(payload,'$.text'),'') <> ''"
        "    THEN json_extract(payload,'$.text') || char(10) || ?2 ELSE ?2 END),"
        "  COALESCE(json_extract(payload,'$.media.path'),'')"
        " FROM media_jobs WHERE id=?1";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(s, 1, job_id);
    sqlite3_bind_text(s, 2, text, -1, SQLITE_STATIC);
    int64_t session_id = -1;
    char *source = NULL, *payload = NULL;
    char media_path[1024] = "";
    if (sqlite3_step(s) == SQLITE_ROW) {
        session_id = sqlite3_column_int64(s, 0);
        const char *v = (const char *)sqlite3_column_text(s, 1);
        source = strdup(v ? v : "");
        v = (const char *)sqlite3_column_text(s, 2);
        payload = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(s, 3);
        if (v) snprintf(media_path, sizeof(media_path), "%s", v);
    }
    sqlite3_finalize(s);
    if (session_id < 0 || !payload) { free(source); free(payload); return -1; }

    int64_t irc = inbox_insert_scanned(db, session_id, source, NULL, payload);
    free(source); free(payload);
    if (irc < 0) {
        LOG_ERROR_("transcribe: inbox insert failed job=%lld — keeping job",
                   (long long)job_id);
        return -1;
    }
    sqlite3_stmt *del;
    if (sqlite3_prepare_v2(db, "DELETE FROM media_jobs WHERE id=?", -1, &del, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(del, 1, job_id);
        sqlite3_step(del); sqlite3_finalize(del);
    }
    /* Job resolved (success or failure text): the spool file is done. */
    if (media_path[0]) unlink(media_path);
    return 0;
}

int llm_transcribe(sqlite3 *db, CURL *curl, int64_t job_id) {
    int64_t session_id = -1;
    int attempts = 0;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT session_id, attempts FROM media_jobs WHERE id=?",
            -1, &s, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(s, 1, job_id);
    if (sqlite3_step(s) == SQLITE_ROW) {
        session_id = sqlite3_column_int64(s, 0);
        attempts = sqlite3_column_int(s, 1);
    }
    sqlite3_finalize(s);
    if (session_id < 0) return -1;   /* row gone — nothing to do */

    MediaJobInfo mj;
    if (media_job_info(db, job_id, &mj) != 0) return -1;

    /* Crash-loop guard: a resubmitted job that keeps dying goes terminal. */
    if (attempts >= TRANSCRIBE_MAX_ATTEMPTS)
        return transcribe_resolve(db, job_id, mj.fail_text);
    sqlite3_stmt *up;
    if (sqlite3_prepare_v2(db, "UPDATE media_jobs SET attempts=attempts+1 WHERE id=?",
                           -1, &up, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(up, 1, job_id);
        sqlite3_step(up); sqlite3_finalize(up);
    }

    const char *db_path = sqlite3_db_filename(db, "main");
    ModelCandidate models[MAX_MODELS];
    int nmodels = model_pick_by_capability(db, mj.cap, models, MAX_MODELS);
    if (nmodels == 0)
        LOG_WARN_("media: no healthy %s-capable model job=%lld",
                  mj.cap, (long long)job_id);

    char *transcript = NULL;
    for (int mi = 0; mi < nmodels && !transcript; mi++) {
        ModelCandidate *m = &models[mi];

        const char *key = m->api_key_env[0] ? getenv(m->api_key_env) : NULL;
        char *key_buf = (key && key[0]) ? strdup(key)
                      : m->api_key_env[0] ? db_secret_get_system(db, m->api_key_env)
                      : NULL;
        if (!key_buf) key_buf = strdup("");

        char *body = transcribe_build_body(db, (EndpointType)m->endpoint_type,
                                           m->model, job_id);
        Config tcfg = {0};
        tcfg.provider.base_url = m->base_url;
        tcfg.provider.model = m->model;
        tcfg.provider.endpoint_type = (EndpointType)m->endpoint_type;
        tcfg.provider.api_key = key_buf;
        char *url = llm_build_url(&tcfg);
        char *auth = llm_build_auth_header(&tcfg);
        if (!body || !url || !auth) {
            free(body); free(url); free(auth); free(key_buf);
            continue;
        }
        const char *headers[] = { "Content-Type: application/json", auth, NULL };

        for (int retry = 0; retry <= TRANSCRIBE_RETRIES && !transcript; retry++) {
            HttpResponse resp = {0};
            HttpRequestOpts opts = {
                .url = url, .method = "POST", .headers = headers,
                .body = body, .curl_handle = curl,
                .max_response_bytes = LLM_RESP_MAX,
            };
            int status = http_do(&opts, &resp);
            LOG_DEBUG_("transcribe: status=%d model=%s job=%lld",
                       status, m->model, (long long)job_id);

            if (status >= 200 && status < 300) {
                transcript = extract_content(db, (EndpointType)m->endpoint_type,
                                             resp.data);
                http_response_free(&resp);
                if (transcript && !transcript[0]) { free(transcript); transcript = NULL; }
                if (transcript) {
                    model_stat_success(db, m->id, 0, 0, 0);
                    break;
                }
                /* 2xx with no text: provider glitch — same transient class
                 * as an empty chat completion. */
                model_stat_error(db, db_path, session_id, m->id, status);
                if (retry < TRANSCRIBE_RETRIES) { sleep(1); continue; }
                break;
            }
            /* Persistent rejections: sideline the model, next candidate. */
            if (status == 400 || status == 401 || status == 403 || status == 404) {
                http_response_free(&resp);
                model_degrade_unavailable(db, db_path, session_id, m->id, status);
                break;
            }
            /* 429/5xx/timeout/network: transient — one retry, then next. */
            http_response_free(&resp);
            model_stat_error(db, db_path, session_id, m->id, status);
            if (retry < TRANSCRIBE_RETRIES) { sleep(1u << retry); continue; }
            break;
        }

        free(body); free(url); free(auth); free(key_buf);
    }

    if (transcript) {
        size_t need = strlen(transcript) + strlen(mj.label) + 8;
        char *text = malloc(need);
        int rc = -1;
        if (text) {
            snprintf(text, need, "%s %s", mj.label, transcript);
            rc = transcribe_resolve(db, job_id, text);
            free(text);
        }
        free(transcript);
        return rc;
    }
    return transcribe_resolve(db, job_id, mj.fail_text);
}
