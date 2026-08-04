#define _POSIX_C_SOURCE 200809L
#include "config_registry.h"
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The registry. Adding a config key means adding a row here — it then exists
 * on every install with a default and a description, no seed data required. */
static const ConfigDef s_defs[] = {
    { "default_agent",      "Assistant",
      "Agent that owns new unrouted sessions" },
    { "max_iterations",     "25",
      "Max tool-loop iterations per turn" },
    { "shell_timeout",      "30",
      "shell_exec timeout in seconds" },
    { "web_port",           "8080",
      "Embedded web server port" },
    { "web_admin_token",    "",
      "Admin dashboard auth token (auto-generated at daemon start; cclaw dashboard prints the URL)" },
    { "max_history_tokens", "0",
      "History token cap per request (0 = derive from context window)" },
    { "stale_lock_timeout", "300",
      "Seconds before a dead process's session locks are reclaimed" },
    { "channel_deaf_timeout", "300",
      "Seconds of received-traffic silence before a live channel runner is"
      " restarted (0 = off; per-channel <ext>.deaf_timeout overrides)" },
    { "cron_min_interval_seconds", "30",
      "Minimum spacing between cron fires (floor on in_seconds/interval_s/"
      "recurring). Default is one daemon tick — fires land within a tick of due" },
    { "cron_max_jobs_per_session", "10",
      "Max cron jobs a session may own (cron_set cap)" },
    { "sql_slow_ms",        "100",
      "Slow-query WARN threshold in ms for the SQL trace (0 = disabled)" },
    { "token_rate_limit",   "1000000",
      "Max LLM tokens per hour (0 = unlimited)" },
    { "save_reasoning",     "0",
      "Store model reasoning text as entries (0 = discard; reasoning is"
      " never replayed to the model either way)" },
    { "daily_cost_limit",   "5.00",
      "Max LLM spend in USD over a rolling 24h window (0 = unlimited;"
      " unpriced models cost 0 — token_rate_limit is the universal brake)" },
    { "context_threshold",  "0.6",
      "Context fill ratio that triggers compaction" },
    { "context_window",     "128000",
      "Global default context window in tokens (overridden per-model in models table)" },
    { "compaction_target",  "0.3",
      "Target context fill ratio after compaction" },
    { "compaction",         "1",
      "Automatic context compaction (1 = on, 0 = off)" },
    { "auto_recall",        "0",
      "Automatic memory recall into context (1 = on, 0 = off)" },
    { "recall_max_tokens",  "500",
      "Token budget for auto-recalled memory" },
    { "approval_timeout_sec", "3600",
      "Seconds before a parked approval expires" },
    { "approval_block_sec", "60",
      "Seconds a foreground approval blocks before backgrounding" },
    { "agent_max_depth",    "2",
      "Max sub-agent nesting depth for launch_agent" },
    { "session_max_active", "20",
      "System-wide in-flight sub-agent sessions — the existence half of the old"
      " AGENT_MAX_TOTAL. launch_agent refuses and session-creating cron fires"
      " skip past it (0 = unlimited)" },
    { "session_max_concurrent", "10",
      "Sessions doing work at once — holding an in-flight LLM request or a"
      " running tool child; parents blocked on sub-agents hold nothing. Past it,"
      " autonomous turn opens defer (inbox rows stay queued); batches with a"
      " human in them always open (0 = unlimited)" },
    { "max_autonomous_turn_streak", "50",
      "Consecutive turns a session may open with no human in the causal chain"
      " (cron fires, sub-agent results, system notices) before turn-open"
      " refuses further autonomous work. The queued inbox rows stay durable —"
      " the session just goes quiet; a human message both gets through and"
      " resets the streak (0 = guard off)" },
    { "agent_delivery_default", "quiescent",
      "Delivery policy seeded on a new child's parent edge when the launcher"
      " has no inheritable policy of its own (root sessions, children of"
      " 'explicit' sessions): iteration | digest | turn | quiescent | explicit"
      " (specs/delivery.md)" },
    { "worker_tools",
      /* D15: workers may delegate too — the recursion rail is agent_max_depth
       * (2) plus the per-parent/system caps, not a missing tool. Omitting
       * launch_agent made the filter do rail-work invisibly, and a parent had
       * no way to see it. The list is rendered into launch_agent's description
       * (llm_payload.c) so a spawner can read what a worker will actually get. */
      "[\"file_read\",\"file_write\",\"shell_exec\",\"web_fetch\",\"js_eval\","
      "\"launch_agent\",\"check_session\",\"search_config\",\"secret_create\"]",
      "Tools a self-spawned worker sub-agent is filtered to when launch_agent"
      " is called without a 'tools' array (JSON array; intersected with the"
      " spawner's grants — it can never widen them)" },
    { "agent_default_tools",
      /* D2 widening (review-1 F12): file navigation + cron self-scheduling
       * are baseline, not escalations. */
      "[\"file_read\",\"file_write\",\"file_list\",\"file_find\",\"file_grep\","
      "\"file_edit\",\"js_eval\",\"request_config\","
      "\"search_config\",\"memory_create\",\"memory_add\",\"memory_edit\","
      "\"memory_delete\",\"cron_set\",\"cron_list\",\"cron_remove\","
      "\"create_agent\",\"update_agent\",\"extension_promote\",\"extension_publish\","
      "\"extension_attach\",\"extension_list\",\"extension_fork\",\"launch_agent\","
      "\"check_session\",\"secret_create\",\"channel_send\"]",
      /* channel_send is safe as a baseline: the grant is not the authority —
       * the route allowlist is (default-deny per target, operator-managed). */
      "Baseline tool grants seeded for a newly created agent (JSON array)" },
    { "agent_default_extensions",
      "[\"cclaw-docs\"]",
      "Extensions attached to every newly created agent (JSON array)" },
    { "agent_approval_tools",
      "[\"extension_publish\"]",
      "Tools that require human approval (approval_mode='always') when granted" },
    { "health_5xx_threshold", "3",
      "5xx errors within the window before a model is marked degraded" },
    { "health_429_threshold", "10",
      "429 errors within the window before a model is marked degraded" },
    { "health_window_sec",  "300",
      "Sliding window in seconds for provider error counting" },
    { "health_cooldown_sec", "300",
      "Seconds a degraded model is skipped before retry" },
    { "llm_response_archive_max", "500",
      "llm_responses rows kept, ok and failures counted separately (0 = archiving off, negative = keep all)" },
    { "inbox_retention_sec", "604800",
      "Seconds to keep consumed inbox rows (0 = keep forever)" },
    { "outbox_retention_sec", "604800",
      "Seconds to keep terminal channel_outbox rows (0 = keep forever)" },
    { "disk_min_free_mb", "20",
      "Free-space floor in MB below which the daemon refuses new LLM dispatch (0 = disabled)" },
    { "workspace",          "",
      "Workspace directory (empty = ~/.cclaw/agents/default/workspace)" },
    { "tmp_root",           "",
      "Parent for agent scratch dirs bind-mounted as /tmp in sandboxed tool"
      " children (empty = the host's /tmp, so temp storage inherits whatever"
      " the host does — tmpfs or disk — and the host's cleaner). Set this to"
      " put scratch on a dedicated volume, or when the host's /tmp is too"
      " small to build in. Layout: <tmp_root>/cclaw-<uid>/<agent>/" },
    { "skills_dirs",
      "[\"~/.cclaw/skills\",\"~/.agents/skills\"]",
      "Skill discovery directories (JSON array, ~ expanded; SKILL.md convention"
      " shared with pi/OpenClaw/Claude Code). Per-agent agents/<name>/skills/"
      " is always scanned too." },
    { "default_primary_model", "",
      "Default primary model for agents without an explicit assignment" },
    { "default_secondary_model", "",
      "Default secondary (fallback) model for agents without an explicit assignment" },
};

const char *config_default(const char *key) {
    if (!key) return NULL;
    for (size_t i = 0; i < sizeof(s_defs) / sizeof(s_defs[0]); i++)
        if (strcmp(s_defs[i].key, key) == 0)
            return s_defs[i].def;
    return NULL;
}

int config_default_int(const char *key) {
    const char *d = config_default(key);
    return d ? atoi(d) : 0;
}

int config_registry_sync(sqlite3 *db) {
    if (!db) return -1;
    /* The C registry owns every non-namespaced key (extension keys are
     * always <ext>.<key>): a key removed from the registry leaves the table
     * on next sync — same drop-first contract as extension_install, so no
     * dead knobs linger in search_config. */
    char keys[4096];
    size_t n = 0;
    keys[n++] = '[';
    for (size_t i = 0; i < sizeof(s_defs) / sizeof(s_defs[0]); i++) {
        int w = snprintf(keys + n, sizeof(keys) - n, "%s\"%s\"",
                         i ? "," : "", s_defs[i].key);
        if (w < 0 || (size_t)w >= sizeof(keys) - n) return -1;
        n += (size_t)w;
    }
    if (n + 2 > sizeof(keys)) return -1;
    keys[n++] = ']';
    keys[n] = '\0';
    sqlite3_stmt *del;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM config WHERE instr(key, '.') = 0 "
            "AND key NOT IN (SELECT value FROM json_each(?1))",
            -1, &del, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(del, 1, keys, -1, SQLITE_STATIC);
    int drc = sqlite3_step(del);
    sqlite3_finalize(del);
    if (drc != SQLITE_DONE) return -1;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO config(key, default_value, description) VALUES(?1, ?2, ?3) "
            "ON CONFLICT(key) DO UPDATE SET default_value=excluded.default_value, "
            "description=excluded.description",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    int rc = 0;
    for (size_t i = 0; i < sizeof(s_defs) / sizeof(s_defs[0]); i++) {
        sqlite3_bind_text(stmt, 1, s_defs[i].key, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, s_defs[i].def, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, s_defs[i].desc, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    return rc;
}

void config_env_name(const char *key, char *buf, size_t cap) {
    size_t n = 0;
    if (cap == 0) return;
    n = (size_t)snprintf(buf, cap, "CCLAW_");
    for (const char *p = key; p && *p && n + 1 < cap; p++, n++)
        buf[n] = (*p == '.') ? '_'
               : (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
    buf[n] = '\0';
}

char *config_get(sqlite3 *db, const char *key) {
    if (!key) return NULL;

    /* Env layer: read live, never persisted (specs/config.md). */
    char envn[160];
    config_env_name(key, envn, sizeof(envn));
    const char *e = getenv(envn);
    if (e) return strdup(e);

    if (db) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(value, default_value), COALESCE(secret,0)"
                " FROM config WHERE key=?1",
                -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
            char *val = NULL;
            int row = 0, secret = 0;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                row = 1;
                secret = sqlite3_column_int(stmt, 1);
                if (!secret) {
                    const char *v = (const char *)sqlite3_column_text(stmt, 0);
                    if (v) val = strdup(v);
                }
            }
            sqlite3_finalize(stmt);
            if (row) {
                /* Secret keys never resolve from config.value — the DB
                 * fallback is the encrypted secrets table under the
                 * canonical env name. */
                if (secret) return db_secret_get_system(db, envn);
                if (val) return val;
                /* row exists but both columns NULL — fall through to registry */
            }
        }
    }
    const char *d = config_default(key);
    return d ? strdup(d) : NULL;
}

int config_get_int(sqlite3 *db, const char *key) {
    char *v = config_get(db, key);
    int n = v ? atoi(v) : 0;
    free(v);
    return n;
}

double config_get_double(sqlite3 *db, const char *key) {
    char *v = config_get(db, key);
    double n = v ? atof(v) : 0.0;
    free(v);
    return n;
}

int config_set(sqlite3 *db, const char *key, const char *value) {
    if (!db || !key) return -1;
    /* Secret keys never land in config.value (specs/config.md). */
    {
        sqlite3_stmt *sk;
        int secret = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(secret,0) FROM config WHERE key=?1",
                -1, &sk, NULL) == SQLITE_OK) {
            sqlite3_bind_text(sk, 1, key, -1, SQLITE_STATIC);
            if (sqlite3_step(sk) == SQLITE_ROW) secret = sqlite3_column_int(sk, 0);
            sqlite3_finalize(sk);
        }
        if (secret) return -1;
    }
    const char *def = config_default(key);
    if (!def) {
        /* Not in the C registry — allowed only if an extension registered it
         * (a config row with a code-owned default_value exists, keyed
         * <ext>.<key> by extension_install). No anonymous config writes. */
        sqlite3_stmt *up;
        if (sqlite3_prepare_v2(db,
                "UPDATE config SET value=?2 WHERE key=?1 AND default_value IS NOT NULL",
                -1, &up, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_text(up, 1, key, -1, SQLITE_STATIC);
        if (value) sqlite3_bind_text(up, 2, value, -1, SQLITE_STATIC);
        else sqlite3_bind_null(up, 2);
        int urc = sqlite3_step(up);
        sqlite3_finalize(up);
        return (urc == SQLITE_DONE && sqlite3_changes(db) == 1) ? 0 : -1;
    }
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO config(key, value, default_value) VALUES(?1, ?2, ?3) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
    if (value) sqlite3_bind_text(stmt, 2, value, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 2);
    sqlite3_bind_text(stmt, 3, def, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}
