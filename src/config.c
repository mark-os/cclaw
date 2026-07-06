#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "config_registry.h"
#include "agent_config.h"
#include "buf.h"
#include "db.h"
#include "log.h"
#include "templates.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

static char *str_dup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char *d = malloc(len);
    if (d) memcpy(d, s, len);
    return d;
}

/* Default workspace, anchored to the DB directory so it can't diverge from the
 * agent tree by launch location. Falls back to a cwd-relative path only when
 * no DB file path is available. Caller owns the returned string. */
static char *default_workspace(const char *dbp) {
    const char *slash = dbp ? strrchr(dbp, '/') : NULL;
    if (slash) {
        char ws[PATH_MAX];
        snprintf(ws, sizeof(ws), "%.*s/agents/default/workspace",
                 (int)(slash - dbp), dbp);
        return str_dup(ws);
    }
    return str_dup(".cclaw/agents/default/workspace");
}

/* See config.h. The agent folder is one level up from agents/<name>/workspace,
 * wherever the workspace was configured; with no workspace we anchor to the DB
 * directory (the same root default_workspace() uses), so the socket still lands
 * in the agent tree rather than a random cwd. */
const char *agent_dir_resolve(const char *workspace, const char *db_path,
                              char *out, size_t cap) {
    if (workspace && workspace[0]) {
        size_t len = strlen(workspace);
        while (len > 1 && workspace[len - 1] == '/') len--;   /* trim trailing / */
        while (len > 0 && workspace[len - 1] != '/') len--;   /* drop last comp  */
        while (len > 1 && workspace[len - 1] == '/') len--;   /* trim separator  */
        if (len > 1 && len < cap) {
            memcpy(out, workspace, len);
            out[len] = '\0';
            return out;
        }
    }
    if (db_path && db_path[0]) {
        const char *slash = strrchr(db_path, '/');
        if (slash) {
            int n = snprintf(out, cap, "%.*s/agents", (int)(slash - db_path), db_path);
            if (n > 0 && (size_t)n < cap) return out;
        }
    }
    snprintf(out, cap, ".cclaw/agents");
    return out;
}

/* Apply env var override: if env set, replace *field */
static void env_override_str(char **field, const char *env_name) {
    const char *val = getenv(env_name);
    if (val) {
        free(*field);
        *field = str_dup(val);
    }
}

static void env_override_int(int *field, const char *env_name) {
    const char *val = getenv(env_name);
    if (val) *field = atoi(val);
}

/* Read file into malloc'd string. Returns NULL if file doesn't exist or on error. */
static const char *DEFAULT_SYSTEM_PROMPT = TPL_DEFAULT_SYSTEM_PROMPT_MD;

static int mkdir_p(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) return -1;
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

int workspace_init(const Config *cfg) {
    if (!cfg || !cfg->workspace) return -1;
    return mkdir_p(cfg->workspace);
}

/* render system prompt with template vars and workspace context */
char *config_render_system_prompt(const Config *cfg, int64_t session_id) {
    const char *tmpl = cfg->system_prompt ? cfg->system_prompt : DEFAULT_SYSTEM_PROMPT;

    /* Build date string */
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char date_buf[11];
    strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &tm);

    /* Build session_id string */
    char sid_buf[21];
    snprintf(sid_buf, sizeof(sid_buf), "%lld", (long long)session_id);

    TemplateVar vars[] = {
        {"{session_id}", sid_buf},
        {"{date}", date_buf},
        {"{workspace}", cfg->workspace ? cfg->workspace : "."},
    };
    char *out = template_render(tmpl, vars, 3);
    return out ? out : str_dup(tmpl);
}

void config_free(Config *cfg) {
    if (!cfg) return;
    free(cfg->provider.base_url);
    free(cfg->provider.api_key);
    free(cfg->provider.model);
    for (size_t i = 0; i < cfg->fallback_count; i++) {
        free(cfg->fallback_providers[i].base_url);
        free(cfg->fallback_providers[i].api_key);
        free(cfg->fallback_providers[i].model);
    }
    free(cfg->fallback_providers);
    free(cfg->db_path);
    free(cfg->workspace);
    free(cfg->system_prompt);
    free(cfg);
}

/* Build Config purely from CCLAW_* env vars.
 * Agent process path — daemon injects all config at fork. */
Config *config_load_from_env(void) {
    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    const char *v;

    /* Provider */
    v = getenv("CCLAW_PROVIDER_API_KEY_ENV");
    if (v && v[0]) {
        const char *key = getenv(v);
        cfg->provider.api_key = str_dup(key ? key : "");
    } else {
        v = getenv("OPENROUTER_API_KEY");
        cfg->provider.api_key = str_dup(v ? v : "");
    }

    v = getenv("CCLAW_PROVIDER_BASE_URL");
    if (!v || !v[0]) v = getenv("CCLAW_PROVIDER");
    cfg->provider.base_url = str_dup(v ? v : CCLAW_DEF_BASE_URL);

    v = getenv("CCLAW_MODEL");
    cfg->provider.model = str_dup(v ? v : CCLAW_DEF_MODEL);

    v = getenv("CCLAW_MAX_TOKENS");
    cfg->provider.max_tokens = v ? atoi(v) : CCLAW_DEF_MAX_TOKENS;

    v = getenv("CCLAW_CONTEXT_WINDOW");
    cfg->provider.context_window = v ? atoi(v) : CCLAW_DEF_CONTEXT_WINDOW;

    /* endpoint type */
    v = getenv("CCLAW_PROVIDER_ENDPOINT_TYPE");
    if (v && strcmp(v, "gemini") == 0)
        cfg->provider.endpoint_type = ENDPOINT_GEMINI;
    else
        cfg->provider.endpoint_type = ENDPOINT_OPENAI;

    /* DB path — env-only loader has no open DB handle to ask, so this is the
     * one place that still reads CCLAW_DB_PATH directly. */
    const char *dbp = getenv("CCLAW_DB_PATH");
    cfg->db_path = str_dup(dbp ? dbp : "cclaw.db");

    /* Workspace — default under ~/.cclaw/agents/default/ for zero-config CLI */
    v = getenv("CCLAW_WORKSPACE");
    cfg->workspace = v ? str_dup(v) : default_workspace(cfg->db_path);

    /* Scalars — defaults come from the config registry so the env-only
     * loader can never drift from the DB loader. */
    v = getenv("CCLAW_MAX_ITERATIONS");
    cfg->max_iterations = v ? atoi(v) : config_default_int("max_iterations");

    v = getenv("CCLAW_MAX_HISTORY_TOKENS");
    cfg->max_history_tokens = v ? atoi(v) : config_default_int("max_history_tokens");

    v = getenv("CCLAW_SHELL_TIMEOUT");
    cfg->shell_timeout = v ? atoi(v) : config_default_int("shell_timeout");

    v = getenv("CCLAW_TOKEN_RATE_LIMIT");
    cfg->token_rate_limit = v ? atoi(v) : config_default_int("token_rate_limit");

    v = getenv("CCLAW_SAVE_REASONING");
    cfg->save_reasoning = v ? atoi(v) : 0;

    v = getenv("CCLAW_SAVE_USAGE");
    cfg->save_usage = v ? atoi(v) : 0;

    v = getenv("CCLAW_SAVE_LOGPROBS");
    cfg->save_logprobs = v ? atoi(v) : 0;

    v = getenv("CCLAW_LOG_LEVEL");
    cfg->log_level = log_level_parse(v);

    v = getenv("CCLAW_CONTEXT_THRESHOLD");
    cfg->context_threshold = v ? (float)atof(v)
                               : (float)config_default_double("context_threshold");

    v = getenv("CCLAW_COMPACTION_TARGET");
    cfg->compaction_target = v ? (float)atof(v)
                               : (float)config_default_double("compaction_target");

    v = getenv("CCLAW_COMPACTION");
    cfg->compaction = v ? atoi(v) : config_default_int("compaction");

    v = getenv("CCLAW_AUTO_RECALL");
    cfg->auto_recall = v ? atoi(v) : config_default_int("auto_recall");

    v = getenv("CCLAW_RECALL_MAX_TOKENS");
    cfg->recall_max_tokens = v ? atoi(v) : config_default_int("recall_max_tokens");

    /* cache_hints */
    v = getenv("CCLAW_CACHE_HINTS");
    if (v) {
        if (strcmp(v, "on") == 0) cfg->provider.cache_hints = CACHE_HINTS_ON;
        else if (strcmp(v, "off") == 0) cfg->provider.cache_hints = CACHE_HINTS_OFF;
        else cfg->provider.cache_hints = CACHE_HINTS_AUTO;
    }

    return cfg;
}

/* Build Config for parent processes (CLI/daemon).
 * Priority: env var > config value > registry default. */
Config *config_load(sqlite3 *db) {
    if (!db) return NULL;

    Config *cfg = calloc(1, sizeof(Config));
    if (!cfg) return NULL;

    /* DB path — ask SQLite for the file it actually opened, not an env var.
     * Downstream consumers (sandbox_mask_state_files, agent_dir_resolve) rely
     * on this being the real path even when CCLAW_DB_PATH was never set. */
    {
        const char *dbfile = sqlite3_db_filename(db, "main");
        cfg->db_path = str_dup((dbfile && dbfile[0]) ? dbfile : "cclaw.db");
    }

    /* Load providers from providers table (ordered by priority) */
    {
        int idx = 0;
        const char *prov_sql = "SELECT name, base_url, endpoint_type, api_key_env,"
                               " default_model, context_window FROM providers ORDER BY priority;";
        sqlite3_stmt *ps;
        if (sqlite3_prepare_v2(db, prov_sql, -1, &ps, NULL) == SQLITE_OK) {
            size_t fb_cap = 4;
            cfg->fallback_providers = calloc(fb_cap, sizeof(ProviderConfig));
            while (sqlite3_step(ps) == SQLITE_ROW) {
                ProviderConfig *p = (idx == 0) ? &cfg->provider : NULL;
                if (idx > 0) {
                    if (cfg->fallback_count >= fb_cap) {
                        fb_cap *= 2;
                        ProviderConfig *tmp = realloc(cfg->fallback_providers,
                            fb_cap * sizeof(ProviderConfig));
                        if (!tmp) break;
                        cfg->fallback_providers = tmp;
                    }
                    p = &cfg->fallback_providers[cfg->fallback_count];
                    memset(p, 0, sizeof(*p));
                }
                const char *v;
                v = (const char *)sqlite3_column_text(ps, 1);
                p->base_url = v ? strdup(v) : strdup("https://openrouter.ai/api/v1");
                v = (const char *)sqlite3_column_text(ps, 2);
                p->endpoint_type = (v && strcmp(v, "gemini") == 0) ? ENDPOINT_GEMINI : ENDPOINT_OPENAI;
                v = (const char *)sqlite3_column_text(ps, 3);
                if (v && v[0]) {
                    const char *key_val = getenv(v);
                    /* env first (user's shell may source a .env), then encrypted kv */
                    p->api_key = (key_val && key_val[0]) ? strdup(key_val)
                                                         : db_secret_get_system(db, v);
                }
                v = (const char *)sqlite3_column_text(ps, 4);
                p->model = v ? strdup(v) : strdup("deepseek/deepseek-v4-flash");
                p->context_window = sqlite3_column_int(ps, 5);
                if (p->context_window <= 0) p->context_window = 128000;
                p->max_tokens = 4096;
                p->cache_hints = CACHE_HINTS_AUTO;
                if (idx > 0) cfg->fallback_count++;
                idx++;
            }
            sqlite3_finalize(ps);
        }
        /* If no providers loaded (empty table OR stale schema), set defaults */
        if (idx == 0) {
            cfg->provider.base_url = strdup("https://openrouter.ai/api/v1");
            cfg->provider.model = strdup("deepseek/deepseek-v4-flash");
            cfg->provider.max_tokens = 4096;
            cfg->provider.context_window = 128000;
            cfg->provider.endpoint_type = ENDPOINT_OPENAI;
            cfg->provider.cache_hints = CACHE_HINTS_AUTO;
            const char *key = getenv("OPENROUTER_API_KEY");
            cfg->provider.api_key = (key && key[0])
                ? strdup(key)
                : db_secret_get_system(db, "OPENROUTER_API_KEY");
        }
    }

    cfg->web_port = config_get_int(db, "web_port");
    cfg->max_iterations = config_get_int(db, "max_iterations");
    cfg->max_history_tokens = config_get_int(db, "max_history_tokens");
    cfg->heartbeat_interval = config_get_int(db, "heartbeat_interval");
    cfg->shell_timeout = config_get_int(db, "shell_timeout");
    cfg->stale_lock_timeout = config_get_int(db, "stale_lock_timeout");
    cfg->token_rate_limit = config_get_int(db, "token_rate_limit");
    cfg->context_threshold = (float)config_get_double(db, "context_threshold");
    cfg->compaction_target = (float)config_get_double(db, "compaction_target");
    cfg->compaction = config_get_int(db, "compaction");
    cfg->auto_recall = config_get_int(db, "auto_recall");
    cfg->recall_max_tokens = config_get_int(db, "recall_max_tokens");

    /* Env var overrides (highest priority) */
    env_override_str(&cfg->provider.api_key, "OPENROUTER_API_KEY");
    env_override_str(&cfg->provider.base_url, "CCLAW_PROVIDER_BASE_URL");
    env_override_str(&cfg->provider.base_url, "CCLAW_PROVIDER");
    env_override_str(&cfg->provider.model, "CCLAW_MODEL");
    env_override_int(&cfg->provider.context_window, "CCLAW_CONTEXT_WINDOW");
    env_override_str(&cfg->system_prompt, "CCLAW_SYSTEM_PROMPT");
    env_override_int(&cfg->web_port, "CCLAW_WEB_PORT");
    env_override_int(&cfg->max_iterations, "CCLAW_MAX_ITERATIONS");
    env_override_int(&cfg->max_history_tokens, "CCLAW_MAX_HISTORY_TOKENS");
    env_override_int(&cfg->heartbeat_interval, "CCLAW_HEARTBEAT_INTERVAL");
    env_override_int(&cfg->shell_timeout, "CCLAW_SHELL_TIMEOUT");
    env_override_int(&cfg->stale_lock_timeout, "CCLAW_STALE_LOCK_TIMEOUT");
    env_override_int(&cfg->save_reasoning, "CCLAW_SAVE_REASONING");
    env_override_int(&cfg->save_usage, "CCLAW_SAVE_USAGE");
    env_override_int(&cfg->save_logprobs, "CCLAW_SAVE_LOGPROBS");
    env_override_int(&cfg->token_rate_limit, "CCLAW_TOKEN_RATE_LIMIT");

    /* endpoint_type env override */
    {
        const char *v = getenv("CCLAW_PROVIDER_ENDPOINT_TYPE");
        if (v && strcmp(v, "gemini") == 0)
            cfg->provider.endpoint_type = ENDPOINT_GEMINI;
    }

    /* compaction env overrides */
    {
        const char *v = getenv("CCLAW_CONTEXT_THRESHOLD");
        if (v) cfg->context_threshold = (float)atof(v);
        v = getenv("CCLAW_COMPACTION_TARGET");
        if (v) cfg->compaction_target = (float)atof(v);
        v = getenv("CCLAW_COMPACTION");
        if (v) cfg->compaction = atoi(v);
    }

    /* auto-recall env overrides */
    env_override_int(&cfg->auto_recall, "CCLAW_AUTO_RECALL");
    env_override_int(&cfg->recall_max_tokens, "CCLAW_RECALL_MAX_TOKENS");

    /* Log level: info default, env override (inherited by worker child).
     * Mirrors config_load_from_env — without this the daemon sat on the
     * calloc'd 0 (= error) and its info lines never reached the journal. */
    cfg->log_level = log_level_parse(getenv("CCLAW_LOG_LEVEL"));

    /* Workspace: DB kv → default, env override (mirrors config_load_from_env).
     * Without this the file tools, proxy mount, and workspace_init all see a
     * NULL workspace and fail with "no workspace configured". */
    {
        char *v = config_get(db, "workspace");
        if (v && v[0]) cfg->workspace = v;
        else { free(v); cfg->workspace = default_workspace(cfg->db_path); }
    }
    env_override_str(&cfg->workspace, "CCLAW_WORKSPACE");

    return cfg;
}


