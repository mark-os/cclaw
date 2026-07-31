#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "config_registry.h"
#include "agent_config.h"
#include "buf.h"
#include "db.h"
#include "log.h"
#include "templates.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

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

/* mkdir one level of the scratch tree, 0700, and refuse anything we don't own.
 * /tmp is mode 1777, so an existing path proves nothing about who made it:
 * O_NOFOLLOW+fstat is the check that matters, not the name. */
static int scratch_mkdir_owned(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    /* O_NOFOLLOW|O_DIRECTORY: refuse a symlink someone planted here, and fstat
     * the object we actually opened rather than re-resolving the name (the
     * name could change between checking it and using it). */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
    if (fd < 0) return -1;
    struct stat st;
    int bad = fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
              st.st_uid != getuid() || (st.st_mode & 07777) != 0700;
    close(fd);
    if (bad) {
        LOG_WARN_("scratch_dir refusing '%s': not a 0700 dir owned by uid %u",
                  path, (unsigned)getuid());
        return -1;
    }
    return 0;
}

/* Empty the abandoned namespace roots. A running child still has its tmpfs
 * mounted on one, so rmdir returns EBUSY and we skip it; a dead child's is
 * empty and goes. That is why nothing needs to remember child pids. */
static void scratch_reap_ns_roots(const char *ns_root) {
    DIR *d = opendir(ns_root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[PATH_MAX];
        int n = snprintf(p, sizeof(p), "%s/%s", ns_root, e->d_name);
        if (n > 0 && (size_t)n < sizeof(p)) rmdir(p);
    }
    closedir(d);
}

/* See config.h. */
const char *scratch_dir_ensure(const char *agent, const char *root_override,
                               char *out, size_t cap) {
    if (!out || cap == 0) return NULL;
    const char *root = (root_override && root_override[0]) ? root_override : "/tmp";
    /* An agent name reaches this from the DB, where it is PascalCase-validated,
     * but a path component is worth checking on its own terms regardless. */
    if (!agent || !agent[0] || strchr(agent, '/') || strcmp(agent, "..") == 0)
        agent = "default";

    char base[PATH_MAX];
    int n = snprintf(base, sizeof(base), "%s/cclaw-%u", root, (unsigned)getuid());
    if (n <= 0 || (size_t)n >= sizeof(base)) return NULL;
    if (scratch_mkdir_owned(base) != 0) return NULL;

    char ns_root[PATH_MAX];
    n = snprintf(ns_root, sizeof(ns_root), "%s/.ns", base);
    if (n > 0 && (size_t)n < sizeof(ns_root) && scratch_mkdir_owned(ns_root) == 0)
        scratch_reap_ns_roots(ns_root);

    n = snprintf(out, cap, "%s/%s", base, agent);
    if (n <= 0 || (size_t)n >= cap) return NULL;
    if (scratch_mkdir_owned(out) != 0) return NULL;
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

static const char *DEFAULT_SYSTEM_PROMPT = TPL_DEFAULT_SYSTEM_PROMPT_MD;

int workspace_init(const Config *cfg) {
    if (!cfg || !cfg->workspace) return -1;
    return util_mkdir_p(cfg->workspace);
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
    free(cfg->provider.name);
    free(cfg->provider.base_url);
    free(cfg->provider.api_key);
    free(cfg->provider.api_key_source);
    free(cfg->provider.model);
    for (size_t i = 0; i < cfg->fallback_count; i++) {
        free(cfg->fallback_providers[i].name);
        free(cfg->fallback_providers[i].base_url);
        free(cfg->fallback_providers[i].api_key);
        free(cfg->fallback_providers[i].api_key_source);
        free(cfg->fallback_providers[i].model);
    }
    free(cfg->fallback_providers);
    free(cfg->db_path);
    free(cfg->workspace);
    free(cfg->system_prompt);
    free(cfg);
}

/* "env:<VAR>" — the provenance label for a key that came from the environment.
 * The variable's *name* is safe to log; its value never is. */
static char *key_source_env(const char *var) {
    size_t n = strlen(var) + 5;
    char *s = malloc(n);
    if (s) snprintf(s, n, "env:%s", var);
    return s;
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
                               " default_model FROM providers ORDER BY priority;";
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
                v = (const char *)sqlite3_column_text(ps, 0);
                p->name = v ? strdup(v) : NULL;
                v = (const char *)sqlite3_column_text(ps, 1);
                p->base_url = v ? strdup(v) : strdup(CCLAW_DEF_BASE_URL);
                v = (const char *)sqlite3_column_text(ps, 2);
                p->endpoint_type = (v && strcmp(v, "gemini") == 0) ? ENDPOINT_GEMINI : ENDPOINT_OPENAI;
                v = (const char *)sqlite3_column_text(ps, 3);
                if (v && v[0]) {
                    const char *key_val = getenv(v);
                    /* env first (user's shell may source a .env), then encrypted kv */
                    if (key_val && key_val[0]) {
                        p->api_key = strdup(key_val);
                        p->api_key_source = key_source_env(v);
                    } else {
                        p->api_key = db_secret_get_system(db, v);
                        if (p->api_key) p->api_key_source = strdup("db:secrets");
                    }
                }
                v = (const char *)sqlite3_column_text(ps, 4);
                p->model = v ? strdup(v) : strdup(CCLAW_DEF_MODEL);
                p->max_tokens = CCLAW_DEF_MAX_TOKENS;
                p->cache_hints = CACHE_HINTS_AUTO;
                if (idx > 0) cfg->fallback_count++;
                idx++;
            }
            sqlite3_finalize(ps);
        }
        /* Key-availability scan: priority order is only a convention — the
         * first provider whose key actually resolves (env or encrypted kv)
         * becomes primary. A keyless head is swapped down into the fallback
         * list so it can still serve if a key appears later.
         *
         * This only decides cfg->provider, which chat routing reads solely for
         * the synthetic candidate llm_req() builds when no keyed model row
         * exists. Routing itself walks the models table and applies the same
         * key test per candidate (provider_key_available in llm_proc.c) —
         * that pairing, not this swap alone, is what keeps a keyless provider
         * out of the request path. */
        if (!cfg->provider.api_key) {
            for (size_t i = 0; i < cfg->fallback_count; i++) {
                if (cfg->fallback_providers[i].api_key) {
                    ProviderConfig tmp = cfg->provider;
                    cfg->provider = cfg->fallback_providers[i];
                    cfg->fallback_providers[i] = tmp;
                    break;
                }
            }
        }
        /* If no providers loaded (empty table OR stale schema), set defaults */
        if (idx == 0) {
            cfg->provider.base_url = strdup(CCLAW_DEF_BASE_URL);
            cfg->provider.model = strdup(CCLAW_DEF_MODEL);
            cfg->provider.max_tokens = CCLAW_DEF_MAX_TOKENS;
            cfg->provider.endpoint_type = ENDPOINT_OPENAI;
            cfg->provider.cache_hints = CACHE_HINTS_AUTO;
            const char *key = getenv("OPENROUTER_API_KEY");
            if (key && key[0]) {
                cfg->provider.api_key = strdup(key);
                cfg->provider.api_key_source = key_source_env("OPENROUTER_API_KEY");
            } else {
                cfg->provider.api_key = db_secret_get_system(db, "OPENROUTER_API_KEY");
                if (cfg->provider.api_key)
                    cfg->provider.api_key_source = strdup("db:secrets");
            }
        }
    }

    cfg->web_port = config_get_int(db, "web_port");
    cfg->context_window = config_get_int(db, "context_window");
    cfg->max_iterations = config_get_int(db, "max_iterations");
    cfg->max_history_tokens = config_get_int(db, "max_history_tokens");
    cfg->shell_timeout = config_get_int(db, "shell_timeout");
    cfg->stale_lock_timeout = config_get_int(db, "stale_lock_timeout");
    cfg->token_rate_limit = config_get_int(db, "token_rate_limit");
    cfg->save_reasoning = config_get_int(db, "save_reasoning");
    /* config_get_double reads the env layer too, so no separate override */
    cfg->daily_cost_limit_nano =
        (int64_t)(config_get_double(db, "daily_cost_limit") * 1e9 + 0.5);
    cfg->context_threshold = (float)config_get_double(db, "context_threshold");
    cfg->compaction_target = (float)config_get_double(db, "compaction_target");
    cfg->compaction = config_get_int(db, "compaction");
    cfg->auto_recall = config_get_int(db, "auto_recall");
    cfg->recall_max_tokens = config_get_int(db, "recall_max_tokens");

    /* Env var overrides (highest priority). No api_key override here: the
     * provider loop already resolves each provider's own api_key_env from the
     * environment first, and a blanket OPENROUTER_API_KEY override would stomp
     * the key of whichever provider the availability scan selected. */
    env_override_str(&cfg->provider.base_url, "CCLAW_PROVIDER_BASE_URL");
    env_override_str(&cfg->provider.base_url, "CCLAW_PROVIDER");
    env_override_str(&cfg->provider.model, "CCLAW_MODEL");
    env_override_int(&cfg->context_window, "CCLAW_CONTEXT_WINDOW");
    env_override_str(&cfg->system_prompt, "CCLAW_SYSTEM_PROMPT");
    env_override_int(&cfg->web_port, "CCLAW_WEB_PORT");
    env_override_int(&cfg->max_iterations, "CCLAW_MAX_ITERATIONS");
    env_override_int(&cfg->max_history_tokens, "CCLAW_MAX_HISTORY_TOKENS");
    env_override_int(&cfg->shell_timeout, "CCLAW_SHELL_TIMEOUT");
    env_override_int(&cfg->stale_lock_timeout, "CCLAW_STALE_LOCK_TIMEOUT");
    env_override_int(&cfg->token_rate_limit, "CCLAW_TOKEN_RATE_LIMIT");

    /* endpoint_type env override */
    {
        /* Both directions: the key-availability scan can leave a native-Gemini
         * row in cfg->provider, so pointing CCLAW_PROVIDER_BASE_URL at an
         * OpenAI-shaped endpoint (a mock, a local server) needs a way back to
         * OpenAI framing. A one-way override silently kept the Gemini wire
         * format and produced no diagnostic. */
        const char *v = getenv("CCLAW_PROVIDER_ENDPOINT_TYPE");
        if (v && strcmp(v, "gemini") == 0)
            cfg->provider.endpoint_type = ENDPOINT_GEMINI;
        else if (v && strcmp(v, "openai") == 0)
            cfg->provider.endpoint_type = ENDPOINT_OPENAI;
        else if (v && v[0])
            LOG_WARN_("CCLAW_PROVIDER_ENDPOINT_TYPE='%s' is not 'openai' or "
                      "'gemini' — ignored", v);
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
     * Without this the daemon sat on the
     * calloc'd 0 (= error) and its info lines never reached the journal. */
    cfg->log_level = log_level_parse(getenv("CCLAW_LOG_LEVEL"));

    /* Workspace: DB kv → default, env override.
     * Without this the file tools, proxy mount, and workspace_init all see a
     * NULL workspace and fail with "no workspace configured". */
    {
        char *v = config_get(db, "workspace");
        if (v && v[0]) { cfg->workspace = v; cfg->workspace_explicit = 1; }
        else { free(v); cfg->workspace = default_workspace(cfg->db_path); }
    }
    if (getenv("CCLAW_WORKSPACE")) cfg->workspace_explicit = 1;
    env_override_str(&cfg->workspace, "CCLAW_WORKSPACE");

    return cfg;
}


