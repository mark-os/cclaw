#define _POSIX_C_SOURCE 200809L
#include "config_registry.h"
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
    { "max_history_tokens", "0",
      "History token cap per request (0 = derive from context window)" },
    { "heartbeat_interval", "0",
      "Daemon heartbeat interval in seconds (0 = disabled)" },
    { "stale_lock_timeout", "300",
      "Seconds before a dead process's session locks are reclaimed" },
    { "token_rate_limit",   "1000000",
      "Max LLM tokens per hour (0 = unlimited)" },
    { "context_threshold",  "0.6",
      "Context fill ratio that triggers compaction" },
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
    { "worker_tools",
      "[\"file_read\",\"file_write\",\"shell_exec\",\"web_fetch\",\"js_eval\","
      "\"check_session\",\"check_approval\",\"search_config\",\"secret_create\"]",
      "Tools granted to self-spawned worker sub-agents (JSON array)" },
    { "agent_default_tools",
      "[\"file_read\",\"file_write\",\"js_eval\",\"request_config\","
      "\"search_config\",\"memory_create\",\"memory_add\",\"memory_edit\","
      "\"memory_delete\",\"configure_provider\",\"configure_channel\","
      "\"create_agent\",\"extension_promote\",\"extension_publish\","
      "\"extension_attach\",\"extension_list\",\"launch_agent\","
      "\"check_session\",\"check_approval\",\"secret_create\"]",
      "Baseline tool grants seeded for a newly created agent (JSON array)" },
    { "health_5xx_threshold", "3",
      "5xx errors within the window before a model is marked degraded" },
    { "health_429_threshold", "10",
      "429 errors within the window before a model is marked degraded" },
    { "health_window_sec",  "300",
      "Sliding window in seconds for provider error counting" },
    { "health_cooldown_sec", "300",
      "Seconds a degraded model is skipped before retry" },
    { "llm_response_archive_max", "500",
      "llm_responses rows kept (0 = archiving off, negative = keep all)" },
    { "workspace",          "",
      "Workspace directory (empty = ~/.cclaw/agents/default/workspace)" },
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

double config_default_double(const char *key) {
    const char *d = config_default(key);
    return d ? atof(d) : 0.0;
}

int config_registry_sync(sqlite3 *db) {
    if (!db) return -1;
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

char *config_get(sqlite3 *db, const char *key) {
    if (db && key) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db,
                "SELECT COALESCE(value, default_value) FROM config WHERE key=?1",
                -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key, -1, SQLITE_STATIC);
            char *val = NULL;
            int row = 0;
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                row = 1;
                const char *v = (const char *)sqlite3_column_text(stmt, 0);
                if (v) val = strdup(v);
            }
            sqlite3_finalize(stmt);
            if (row) {
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
    const char *def = config_default(key);
    if (!def) return -1;   /* unregistered key — no anonymous config writes */
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
