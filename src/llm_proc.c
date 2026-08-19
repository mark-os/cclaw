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
/* Generous ceiling on any provider response body — a real completion is a few
 * MB at most; this only bounds daemon OOM from a malicious/buggy provider
 * (web_fetch/js_http already cap their own). */
#define LLM_RESP_MAX (32u * 1024 * 1024)
/* Hard ceiling on the apply-time probe. It runs synchronously in the approval
 * path, so this is also the worst-case event-loop stall — deliberately short,
 * and a timeout counts as a failed probe (config-ax Phase 2A). */
#define LLM_PROBE_TIMEOUT_SEC 15



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

/* Defined below with the rest of the degradation machinery; load_candidates is
 * the one caller that runs *before* a request exists. */
static void model_degrade_config(sqlite3 *db, const char *model_id,
                                 const char *api_key_env);
static void model_config_recover(sqlite3 *db, const char *model_id);

/* The agent's explicit list (agent_models, pos order) IS the routing policy —
 * nothing is appended after it (plan/projects/model-routing.md R1). Degraded
 * rows are NOT filtered here: health is consulted at selection time inside
 * llm_req, because health reorders the list but never empties it (R4). Only
 * 'disabled' (an operator statement) and missing-credential rows drop out. */
static int load_candidates(sqlite3 *db, const char *agent_name,
                           ModelCandidate *out, int max) {
    if (max <= 0) return 0;
    int n = 0;

    const char *sql =
        "SELECT m.id, m.model, p.base_url, p.api_key_env, p.endpoint_type,"
        "       m.context_window"
        " FROM agent_models am"
        " JOIN models m ON m.id = am.model_id"
        " JOIN providers p ON p.name = m.provider_name"
        " WHERE am.agent_name = ?1 AND m.status != 'disabled'"
        " ORDER BY am.pos";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(s, 1, agent_name ? agent_name : "", -1, SQLITE_STATIC);
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
        /* A named candidate dropped for a missing key is the 2026-08-10
         * silent-reroute: it never reached the request, so nothing recorded
         * it. Record it through the degrade machinery instead (A8). */
        if (provider_key_available(db, c->api_key_env)) {
            model_config_recover(db, c->id);
            n++;
        } else {
            LOG_WARN_("llm: candidate %s dropped — api_key_env %s resolves to "
                      "nothing (agent=%s)", c->id, c->api_key_env,
                      agent_name ? agent_name : "?");
            model_degrade_config(db, c->id, c->api_key_env);
        }
    }
    sqlite3_finalize(s);
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
        " ORDER BY m.created_at, m.id";
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

/* One counter drives degradation (model-routing.md R3): any transient failure
 * increments consec_failures, any success zeroes it. At or past
 * health_fail_threshold the cooldown is stamped UNCONDITIONALLY — every
 * further failure re-stamps it, so a still-dead model keeps getting
 * re-sidelined after its cooldown lapses instead of being retried with the
 * full ladder forever (the 2026-08-19 one-shot bug: the old transition guard
 * required status='healthy', which only a success could restore).
 * No per-degrade operator message — the serving-model-change notice after the
 * next success is what the user actually experiences (R6). */
static void model_stat_error(sqlite3 *db, const char *model_id, int status) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE models SET total_requests=total_requests+1,"
            " consec_failures=consec_failures+1, last_error_at=unixepoch()"
            " WHERE id=?1", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s); sqlite3_finalize(s);

    if (sqlite3_prepare_v2(db,
            "UPDATE models SET status='degraded',"
            " degraded_until=unixepoch()+(SELECT CAST(COALESCE(value, default_value, '300') AS INTEGER)"
            "   FROM config WHERE key='health_cooldown_sec')"
            " WHERE id=?1 AND status != 'disabled'"
            " AND consec_failures >= (SELECT CAST(COALESCE(value, default_value, '4') AS INTEGER)"
            "   FROM config WHERE key='health_fail_threshold')",
            -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s);
    if (sqlite3_changes(db) > 0)
        LOG_INFO_("model degraded model=%s status=%d", model_id, status);
    sqlite3_finalize(s);
}

/* A Retry-After beyond the backoff scale is the server saying "go away for
 * real": degrade immediately without burning the remaining attempts, honoring
 * the longer of the server's ask and our own cooldown. Counter jumps to the
 * threshold so the row reads coherently (it IS at its failure limit). */
static void model_degrade_retry_after(sqlite3 *db, const char *model_id,
                                      int retry_after) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE models SET status='degraded', last_error_at=unixepoch(),"
            " consec_failures=MAX(consec_failures+1,"
            "   (SELECT CAST(COALESCE(value, default_value, '4') AS INTEGER)"
            "     FROM config WHERE key='health_fail_threshold')),"
            " degraded_until=unixepoch()+MAX(?2,"
            "   (SELECT CAST(COALESCE(value, default_value, '300') AS INTEGER)"
            "     FROM config WHERE key='health_cooldown_sec'))"
            " WHERE id=?1 AND status != 'disabled'", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(s, 2, retry_after);
    sqlite3_step(s);
    if (sqlite3_changes(db) > 0)
        LOG_INFO_("model degraded model=%s reason=retry_after=%ds", model_id, retry_after);
    sqlite3_finalize(s);
}

/* Auth/not-found/bad-request don't self-heal within a turn: sideline for a
 * cooldown so the model isn't retried every request. The cooldown still
 * re-probes periodically in case the key/model was fixed. */
static void model_degrade_unavailable(sqlite3 *db, const char *model_id,
                                      int status) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE models SET status='degraded', last_error_at=unixepoch(),"
            " degraded_until=unixepoch()+(SELECT CAST(COALESCE(value, default_value, '300') AS INTEGER)"
            "   FROM config WHERE key='health_cooldown_sec')"
            " WHERE id=?1 AND status != 'disabled'", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s);
    if (sqlite3_changes(db) > 0)
        LOG_INFO_("model degraded model=%s status=%d reason=unavailable", model_id, status);
    sqlite3_finalize(s);
}

/* Health at selection time: an active error cooldown. Config degradation
 * (NULL degraded_until) never blocks selection — those candidates were
 * already dropped by the key check in load_candidates. */
static int model_degraded_now(sqlite3 *db, const char *model_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM models WHERE id=?1 AND status='degraded'"
            " AND degraded_until IS NOT NULL AND degraded_until > unixepoch()",
            -1, &s, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    int hit = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return hit;
}

/* Config-shaped degradation: the model can't be routed to at all because its
 * provider's api_key_env resolves to neither an env var nor a system secret.
 * Not an error count — the request never happened — so the state is set
 * directly on the healthy→degraded transition.
 *
 * degraded_until stays NULL, and that is the marker: every error-driven
 * degradation sets a cooldown, so "degraded with no cooldown" means "degraded
 * by configuration", which model_config_recover is allowed to clear the moment
 * the key resolves again. Routing is unaffected either way — the candidate is
 * dropped by the key check itself. */
static void model_degrade_config(sqlite3 *db, const char *model_id,
                                 const char *api_key_env) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE models SET status='degraded', degraded_until=NULL"
            " WHERE id=?1 AND status='healthy'", -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    if (changed <= 0) return;
    LOG_INFO_("model degraded model=%s reason=config key=%s", model_id, api_key_env);
}

/* The recovery half of model_degrade_config: the key resolves again, so undo
 * the config degradation — and only that one (see the degraded_until marker).
 * Mirrors model_stat_success's "success is recovery" semantics for a candidate
 * that never gets as far as a request. */
static void model_config_recover(sqlite3 *db, const char *model_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "UPDATE models SET status='healthy'"
            " WHERE id=?1 AND status='degraded' AND degraded_until IS NULL",
            -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, model_id, -1, SQLITE_STATIC);
    sqlite3_step(s);
    if (sqlite3_changes(db) > 0)
        LOG_INFO_("model recovered model=%s reason=config", model_id);
    sqlite3_finalize(s);
}

static void model_stat_success(sqlite3 *db, const char *model_id,
                               int tokens_in, int tokens_out, int64_t cost_nano) {
    /* Success is recovery: restore 'healthy' so the degradation guard in
     * model_stat_error can fire again on the next real outage. */
    const char *sql = "UPDATE models SET total_requests=total_requests+1,"
        " total_tokens_in=total_tokens_in+?, total_tokens_out=total_tokens_out+?,"
        " total_cost_nano=total_cost_nano+?, last_success_at=unixepoch(),"
        " consec_failures=0,"
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

/* ── llm_probe_agent: does the new config actually serve? ─────── */

/* Smallest legal completion request for this endpoint. Built by SQLite so the
 * model name is escaped by the JSON writer, like transcribe_build_body. */
static char *probe_build_body(sqlite3 *db, EndpointType ep, const char *model) {
    const char *sql = (ep == ENDPOINT_GEMINI)
        ? "SELECT json_object('contents', json_array(json_object("
          "  'role','user','parts', json_array(json_object('text','ping')))),"
          " 'generationConfig', json_object('maxOutputTokens', 1))"
        : "SELECT json_object('model', ?1, 'messages',"
          " json_array(json_object('role','user','content','ping')),"
          " 'max_tokens', 1)";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    if (ep != ENDPOINT_GEMINI)
        sqlite3_bind_text(s, 1, model ? model : "", -1, SQLITE_STATIC);
    char *body = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t) body = strdup(t);
    }
    sqlite3_finalize(s);
    return body;
}

/* The model id the provider says answered ($.model), falling back to the id we
 * asked for — intent and effect are exactly what the probe exists to separate. */
static void probe_served_model(sqlite3 *db, const char *body, const char *asked,
                               char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s", asked ? asked : "");
    if (!body) return;
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT json_extract(?1,'$.model') WHERE json_valid(?1)",
            -1, &s, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(s, 1, body, -1, SQLITE_STATIC);
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v && v[0]) snprintf(out, out_sz, "%s", v);
    }
    sqlite3_finalize(s);
}

int llm_probe_agent(sqlite3 *db, const char *agent_name, int64_t session_id,
                    char *served, size_t served_sz, char *reason, size_t reason_sz) {
    if (served && served_sz) served[0] = '\0';
    if (reason && reason_sz) reason[0] = '\0';

    /* Whoever would serve the very next request — same loader, same order. */
    ModelCandidate m;
    if (load_candidates(db, agent_name, &m, 1) != 1) {
        snprintf(reason, reason_sz, "no routable model for this agent");
        return -1;
    }

    const char *key = m.api_key_env[0] ? getenv(m.api_key_env) : NULL;
    char *key_buf = (key && key[0]) ? strdup(key)
                  : m.api_key_env[0] ? db_secret_get_system(db, m.api_key_env)
                  : NULL;
    if (!key_buf) key_buf = strdup("");

    Config pcfg = {0};
    pcfg.provider.base_url = m.base_url;
    pcfg.provider.model = m.model;
    pcfg.provider.endpoint_type = (EndpointType)m.endpoint_type;
    pcfg.provider.api_key = key_buf;

    char *body = probe_build_body(db, (EndpointType)m.endpoint_type, m.model);
    char *url = llm_build_url(&pcfg);
    char *auth = llm_build_auth_header(&pcfg);
    int rc = -1;
    if (!body || !url || !auth) {
        snprintf(reason, reason_sz, "could not build the request");
        goto done;
    }

    const char *headers[] = { "Content-Type: application/json", auth, NULL };
    HttpResponse resp = {0};
    HttpRequestOpts opts = {
        .url = url, .method = "POST", .headers = headers, .body = body,
        .timeout = LLM_PROBE_TIMEOUT_SEC,
        .max_response_bytes = LLM_RESP_MAX,
    };
    int status = http_do(&opts, &resp);

    /* One request, one verdict — no retry ladder, no next candidate: a probe
     * that needs a second chance has already answered the question. The
     * archive is the only DB write, so `cclaw resp` can show what came back;
     * no entries, no turn state, no model stats (this is a config event, not
     * traffic the routing health model should learn from). */
    char label[32];
    if (status == -2)       snprintf(label, sizeof(label), "probe_timeout");
    else if (status < 0)    snprintf(label, sizeof(label), "probe_network_error");
    else if (status < 200 || status >= 300)
                            snprintf(label, sizeof(label), "probe_http_%d", status);
    else if (!resp.data || !resp.data[0])
                            snprintf(label, sizeof(label), "probe_empty");
    else                    snprintf(label, sizeof(label), "probe_ok");

    db_archive_response(db, session_id, db_next_iteration_id(db, session_id),
                        m.id, label, resp.data ? resp.data : resp.err_detail, body);

    if (status == -2)
        snprintf(reason, reason_sz, "timed out after %ds", LLM_PROBE_TIMEOUT_SEC);
    else if (status < 0)
        snprintf(reason, reason_sz, "transport error reaching %s", m.base_url);
    else if (status < 200 || status >= 300)
        snprintf(reason, reason_sz, "http %d from %s", status, m.base_url);
    else if (!resp.data || !resp.data[0])
        snprintf(reason, reason_sz, "empty response from %s", m.base_url);
    else {
        probe_served_model(db, resp.data, m.id, served, served_sz);
        rc = 0;
    }
    LOG_INFO_("probe %s model=%s agent=%s status=%d",
              rc == 0 ? "ok" : "failed", m.id, agent_name ? agent_name : "?", status);
    http_response_free(&resp);

done:
    free(body); free(url); free(auth); free(key_buf);
    return rc;
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
     * memory blocks, running sub-agents, open approvals) — never baked
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
    char served_id[128] = "";   /* candidate that answered, for the R6 notice */

    /* One iteration id for this LLM request: every raw response archived below
     * (the failed attempts and the final ingest) shares it. db_next_iteration_id
     * is a pure read, so it stays stable until the success path inserts entries.
     * The *turn* this iteration belongs to is carried by the entries themselves
     * (entries.turn_id, filled from the parent by entries_turn_ai). */
    int64_t iteration_id = db_next_iteration_id(db, session_id);

    /* ── Selection loop (model-routing.md R3/R4): degradation IS the retry
     * policy. Each pass picks the first candidate in list order that isn't
     * skipped for this request, isn't inside an error cooldown, and hasn't
     * hit the failure threshold locally; a model keeps getting attempts only
     * while it's the best eligible candidate, and its own consec_failures
     * crossing the threshold is what dethrones it. When every candidate is
     * cooling down, the first one still gets ONE live attempt (health
     * reorders the list, never empties it — and that attempt doubles as the
     * early-recovery probe). Backoff 1s/2s/4s applies only between
     * consecutive attempts on the SAME model; switching costs nothing. */
    int fail_threshold = config_get_int(db, "health_fail_threshold");
    if (fail_threshold <= 0) fail_threshold = 4;
    int attempts[MAX_MODELS] = {0};
    int skip[MAX_MODELS] = {0};   /* prompt-specific / rejected: out for this request */
    int desperation_done = 0;
    int last_mi = -1, same_run = 0;
    int next_wait = 0;            /* short Retry-After, replaces the ladder step */

    while (!llm_ok && !had_dberr) {
        int mi = -1;
        for (int i = 0; i < nmodels; i++)
            if (!skip[i] && attempts[i] < fail_threshold &&
                !model_degraded_now(db, models[i].id)) { mi = i; break; }
        if (mi < 0 && !desperation_done) {
            for (int i = 0; i < nmodels; i++)
                if (!skip[i] && attempts[i] == 0) { mi = i; desperation_done = 1; break; }
        }
        if (mi < 0) break;
        ModelCandidate *m = &models[mi];

        if (mi == last_mi) {
            same_run++;
            int step = same_run <= 3 ? (1 << (same_run - 1)) : 4;
            sleep((unsigned)(next_wait > 0 ? next_wait : step));
        } else {
            same_run = 0;
        }
        next_wait = 0;
        last_mi = mi;
        attempts[mi]++;

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

        /* Stash request body on the heap: payload.body is only valid while
         * the stmt is open, but we must release the stmt (drop TRANS_READ)
         * before any DB write, and archiving a failed attempt writes. */
        char *req_body = payload.body ? strdup(payload.body) : NULL;
        llm_payload_release(&payload);

        /* Build URL + auth */
        char *url = llm_build_url(&route_cfg);
        char *auth = llm_build_auth_header(&route_cfg);
        if (!url || !auth) { free(url); free(auth); free(req_body); free(key_buf); continue; }

        char session_hdr[64];
        snprintf(session_hdr, sizeof(session_hdr), "x-session-id: cclaw-%lld", (long long)session_id);
        const char *headers[] = { "Content-Type: application/json", auth, session_hdr, NULL };

        /* One attempt. Classification (specs/error-handling.md):
         *   transient (E1/E2/E3/E4/E6/E10) -> count toward the model's
         *     consec_failures and loop (selection decides who's next);
         *   Retry-After > 4s -> immediate degrade, no more attempts here;
         *   permanent for this model (E5/E11/E12/E13) -> local skip, and
         *     E11/E12/other-400 also degrade with a cooldown;
         *   our-side fatal (E14) -> abort the turn. */
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
        LOG_DEBUG_("llm_req: %ldms status=%d model=%s attempt=%d", elapsed, status,
                   m->model, attempts[mi]);
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

        if (status == 429 || (status >= 500 && status < 600)) {
            fail_text = status == 429 ? "error: rate limited"
                                      : "error: provider server error";
            if (status == 429 && resp.retry_after > 4) {
                model_degrade_retry_after(db, m->id, resp.retry_after);
            } else {
                if (status == 429 && resp.retry_after > 0)
                    next_wait = resp.retry_after;
                model_stat_error(db, m->id, status);
            }
            http_response_free(&resp);
        } else if (status == 400 && llm_is_context_overflow(resp.data)) {
            /* Prompt-specific — the model's fault it doesn't fit, not its
             * health. No count. */
            LOG_DEBUG_("llm_req model_skip model=%s reason=context_overflow", m->model);
            fail_text = "error: prompt too large for the model's context window";
            skip[mi] = 1;
            http_response_free(&resp);
        } else if (status == 400) {
            /* Persistent rejection (bad payload shape, encoding issues) —
             * degrade so we don't retry every turn. */
            LOG_DEBUG_("llm_req model_skip model=%s reason=bad_request", m->model);
            fail_text = "error: provider rejected the request";
            skip[mi] = 1;
            model_degrade_unavailable(db, m->id, status);
            http_response_free(&resp);
        } else if (status == 401 || status == 403 || status == 404) {
            LOG_DEBUG_("llm_req model_skip model=%s reason=%s", m->model,
                       status == 404 ? "not_found" : "auth_failed");
            fail_text = status == 404 ? "error: model not available"
                                      : "error: authentication failed";
            skip[mi] = 1;
            model_degrade_unavailable(db, m->id, status);
            http_response_free(&resp);
        } else if (status == -2) {
            fail_text = "error: request timed out";
            model_stat_error(db, m->id, status);
            http_response_free(&resp);
        } else if (status < 0) {
            fail_text = "error: network error";
            model_stat_error(db, m->id, status);
            http_response_free(&resp);
        } else if (status < 200 || status >= 300) {
            /* Other 4xx: the request itself was rejected — resending it to
             * the same model can't change the answer; another candidate
             * might. No count (E13). */
            fail_text = "error: provider rejected the request";
            skip[mi] = 1;
            http_response_free(&resp);
        } else if (!resp.data || !resp.data[0]) {
            /* A 2xx with an empty body is a provider/gateway hiccup (often a
             * slow near-timeout response). db_ingest_response would bail on
             * the NULL body *before* archiving, leaving an opaque error with
             * no forensic trail. Archive it (with the request we sent). */
            LOG_INFO_("llm_req empty_body model=%s attempt=%d", m->model, attempts[mi]);
            db_archive_response(db, session_id, iteration_id, m->id, "empty",
                                resp.data, req_body);
            fail_text = "error: provider returned an empty response";
            model_stat_error(db, m->id, status);
            http_response_free(&resp);
        } else {
            /* ── Ingest response straight to the DB ── */
            TypedIngestResult ir;
            LlmRespStatus st = db_ingest_response(db, session_id, iteration_id,
                                   m->id, route_cfg.provider.endpoint_type,
                                   resp.data, req_body, route_cfg.save_reasoning, &ir);
            http_response_free(&resp);

            if (st == LLM_RESP_OK) {
                model_stat_success(db, m->id, ir.prompt_tokens, ir.completion_tokens, ir.cost_nano);
                snprintf(served_id, sizeof(served_id), "%s", m->id);
                llm_ok = 1;
            } else if (st == LLM_RESP_DBERR) {
                /* Our DB failed (SQLITE_BUSY) — body was valid. Diagnostics
                 * already logged by db_ingest_response. */
                had_dberr = 1;
            } else {
                /* EMPTY (zero-usage stop, archived 'empty') and MALFORMED
                 * (bad body, archived 'malformed') are provider glitches
                 * wrapped in a 2xx: transient. */
                LOG_INFO_("llm_req %s model=%s attempt=%d",
                          st == LLM_RESP_EMPTY ? "empty_completion" : "malformed_body",
                          m->model, attempts[mi]);
                fail_text = st == LLM_RESP_EMPTY
                    ? "error: provider returned an empty response"
                    : "error: provider returned a malformed response";
                model_stat_error(db, m->id, status);
            }
        }

        free(url); free(auth);
        free(req_body);
        free(key_buf);
    }

    /* R6 notice: the model serving this agent changed. Derived from the
     * archive (last two ok rows for this agent), never from new state; fires
     * on any transition — fallback, recovery, config change — because "who is
     * answering me" is the thing the operator can't otherwise see. Cause
     * attached when the previous server is inside an error cooldown. */
    if (llm_ok && served_id[0] && config_get_int(db, "notify_model_change")) {
        char prev_id[128] = "";
        sqlite3_stmt *ps;
        if (sqlite3_prepare_v2(db,
                "SELECT r.model FROM llm_responses r JOIN sessions s ON r.session_id=s.id"
                " WHERE s.agent_name=(SELECT agent_name FROM sessions WHERE id=?1)"
                " AND r.status='ok' ORDER BY r.id DESC LIMIT 1 OFFSET 1",
                -1, &ps, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ps, 1, session_id);
            if (sqlite3_step(ps) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(ps, 0);
                if (v) snprintf(prev_id, sizeof(prev_id), "%s", v);
            }
            sqlite3_finalize(ps);
        }
        if (prev_id[0] && strcmp(prev_id, served_id) != 0) {
            char text[512];
            if (model_degraded_now(db, prev_id))
                snprintf(text, sizeof(text),
                         "now serving via %s (%s degraded — will switch back "
                         "when it recovers)", served_id, prev_id);
            else
                snprintf(text, sizeof(text),
                         "now serving via %s (was %s)", served_id, prev_id);
            channel_notify_session(db, cfg->db_path, session_id, text);
        }
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

/* The mechanical carry-over (E2): the commitments a prose summary is most
 * likely to drop, read straight from the DB in one pass. Same three live-state
 * sources (and wording) the turn-fresh <RELEVANT_CONTEXT> block uses
 * (llm_payload.c), plus this agent's armed one-shots. Every section
 * COALESCE'd away when empty; all-empty yields '' and no coda at all. */
static const char SQL_STATE_CODA[] =
    "WITH appr AS ("
    /* Pending only — decided rows are history, not carryable state (see the
     * matching rationale in llm_payload.c). */
    "  SELECT group_concat("
    "    '  #' || a.id || ' ' || COALESCE(a.tool_name, a.action, '?') ||"
    "    ' — pending: waiting on a human decision', char(10)) AS txt"
    "  FROM approvals a WHERE a.session_id=?1 AND a.state='pending'"
    "), sub AS ("
    "  SELECT group_concat("
    "    '  session #' || s.id || ' (' || COALESCE(s.agent_name,'?') || ') — ' || s.state,"
    "    char(10)) AS txt"
    "  FROM sessions s WHERE s.parent_session_id=?1 AND s.state != 'idle'"
    "), one AS ("
    /* One-shots only (run_at set): a recurring job re-announces itself every
     * fire, but a one-shot fires once and is gone — losing it loses the plan. */
    "  SELECT group_concat("
    "    '  ' || c.name || ' — fires ' || datetime(c.next_run_at,'unixepoch') || 'Z: ' || c.task,"
    "    char(10)) AS txt"
    "  FROM cron_jobs c"
    "  WHERE c.agent_name=(SELECT agent_name FROM sessions WHERE id=?1)"
    "    AND c.enabled=1 AND c.run_at IS NOT NULL AND c.next_run_at > unixepoch()"
    ")"
    "SELECT COALESCE('open approvals:' || char(10) || appr.txt || char(10), '') ||"
    "       COALESCE('running sub-agents:' || char(10) || sub.txt || char(10), '') ||"
    "       COALESCE('scheduled one-shots:' || char(10) || one.txt || char(10), '')"
    " FROM appr, sub, one;";

char *compaction_state_coda(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, SQL_STATE_CODA, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(s, 1, session_id);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        if (t && t[0]) {
            size_t n = strlen(t) + sizeof(COMPACTION_CODA_MARKER) + 2;
            r = malloc(n);
            if (r) snprintf(r, n, "%s\n%s", COMPACTION_CODA_MARKER, t);
        }
    }
    sqlite3_finalize(s);
    return r;
}

/* One compaction attempt. *budget_out gets the target token budget as soon as
 * it is known (0 if we failed before that) — the wrapper quotes it in the
 * operator notice. */
static int compaction_attempt(sqlite3 *db, CURL *curl, int64_t session_id,
                              const char *agent_name, int *budget_out) {
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
    *budget_out = target_tokens;

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

    /* E2: the model summarizes prose; live commitments are carried
     * mechanically. Queried AFTER the call returns and appended here, so the
     * compaction model never handles them — it cannot drop or paraphrase what
     * it never saw. Still exactly one role-4 entry. */
    char *coda = compaction_state_coda(db, session_id);
    char *content = summary;
    if (coda) {
        size_t n = strlen(summary) + strlen(coda) + 8;
        char *joined = malloc(n);
        if (joined) {
            snprintf(joined, n, "%s\n\n%s", summary, coda);
            content = joined;
        }
        free(coda);
    }

    /* Call entry_compact to insert summary and reparent */
    int64_t compact_id = entry_compact(db, session_id, last_kept_id, first_after_id, content);
    if (content != summary) free(content);
    free(summary);

    context_plan_free(&plan);
    config_free(cfg);

    return (compact_id > 0) ? 0 : -1;
}

/* One session-id-bound statement, stepped and finalized. */
static void session_exec1(sqlite3 *db, const char *sql, int64_t session_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return;
    sqlite3_bind_int64(s, 1, session_id);
    sqlite3_step(s);
    sqlite3_finalize(s);
}

/* C4: a session whose compaction keeps failing is silently running on the
 * read-time window (plan_find_cut drops old turns from the request; nothing is
 * deleted). The agent is deliberately not told — it cannot fix it, and telling
 * it invites coping behavior — so the notice goes to the operator's channel,
 * once per failure streak (the transition to COMPACTION_FAIL_NOTIFY, the
 * status='healthy' guard's pattern). Channel-less sessions no-op. */
void compaction_record_outcome(sqlite3 *db, const char *db_path,
                               int64_t session_id, int ok, int budget_tokens) {
    session_exec1(db, ok ? "UPDATE sessions SET compaction_fail_count=0"
                           " WHERE id=?1;"
                         : "UPDATE sessions"
                           " SET compaction_fail_count=compaction_fail_count+1"
                           " WHERE id=?1;", session_id);
    if (ok) return;
    if (db_scalar_i64(db, "SELECT compaction_fail_count FROM sessions WHERE id=?1;",
                      session_id, 0) != COMPACTION_FAIL_NOTIFY)
        return;                                  /* fire on the transition only */

    char text[256];
    snprintf(text, sizeof(text),
             "⚠ session #%lld compaction failing — context is sliding-window "
             "around %d tokens; older turns invisible to the model until "
             "compaction recovers.",
             (long long)session_id, budget_tokens);
    channel_notify_session(db, db_path, session_id, text);
}

int llm_compaction(sqlite3 *db, CURL *curl, int64_t session_id, const char *agent_name) {
    int budget = 0;
    int rc = compaction_attempt(db, curl, session_id, agent_name, &budget);
    compaction_record_outcome(db, sqlite3_db_filename(db, "main"), session_id,
                              rc == 0, budget);
    return rc;
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
                model_stat_error(db, m->id, status);
                if (retry < TRANSCRIBE_RETRIES) { sleep(1); continue; }
                break;
            }
            /* Persistent rejections: sideline the model, next candidate. */
            if (status == 400 || status == 401 || status == 403 || status == 404) {
                http_response_free(&resp);
                model_degrade_unavailable(db, m->id, status);
                break;
            }
            /* 429/5xx/timeout/network: transient — one retry, then next. */
            http_response_free(&resp);
            model_stat_error(db, m->id, status);
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
