#ifndef CCLAW_AGENT_CONFIG_H
#define CCLAW_AGENT_CONFIG_H

#include <stddef.h>
#include <sqlite3.h>
#include "types.h"

/* V124: system defaults — single source of truth for agent creation,
 * CLI zero-config, and absent-key resolution in daemon fork */
#define AGENT_DEFAULT_MAX_ITERATIONS 25
#define AGENT_DEFAULT_SHELL_TIMEOUT  30

/* V119: default tool whitelist */
#define AGENT_DEFAULT_TOOLS \
    "file_read", "file_write", "js_eval", \
    "memory_create", "memory_append", "memory_replace", \
    "request_config"
#define AGENT_DEFAULT_TOOLS_COUNT 7

/* Per-agent config loaded from agents table in cclaw.db.
 * Fields override global Config when non-NULL/non-zero. */
typedef struct {
    char *name;             /* agent name */
    char *model;            /* model override (NULL = use global) */
    char **tools;           /* tool whitelist (NULL = all tools) */
    size_t tool_count;
    char **allowed_hosts;   /* V38: hostnames for http_fetch */
    size_t allowed_hosts_count;
    char **read_access;     /* V66: extra dirs granted read-only in namespace sandbox */
    size_t read_access_count;
    int max_iterations;     /* 0 = use global */
} AgentConfig;

/* Load agent config from cclaw.db agents table.
 * Returns NULL if agent not found. */
AgentConfig *agent_config_load_db(sqlite3 *db, const char *name);

/* Free AgentConfig. */
void agent_config_free(AgentConfig *ac);

/* T77: Load system prompt from agents/<name>/system.md, render template vars
 * {session_id}, {date}, {agent_name}. Returns heap-allocated string.
 * Returns NULL if file missing (caller should fall back to global system_prompt). */
char *agent_load_system_prompt(const char *agents_dir, const char *name,
                               int64_t session_id);

/* T80: Load all skills from agents/<name>/skills/ (.md files), concatenate content.
 * Returns heap-allocated string (newline-separated skill contents).
 * Returns NULL if no skills dir or no .md files found. */
char *agent_load_skills(const char *agents_dir, const char *name);

/* T122: Assemble system prompt from DB agent row (template + soul + memory + skills).
 * Returns heap-allocated string. Caller must free. */
char *agent_build_system_prompt(sqlite3 *db, const char *agent_name,
                                int64_t session_id, const char *agents_dir,
                                const Config *fallback_cfg);

/* T186: Create an ephemeral agent directory with workspace.
 * Returns heap-allocated agent name on success (caller frees), NULL on failure. */
char *agent_create_ephemeral(const char *agents_dir, sqlite3 *db);

/* T144/T196: Add host to agent's allowed_hosts. Returns 0 on success. */
int agent_config_add_host(sqlite3 *db, const char *name, const char *host);

/* T144/T196: Remove host from agent's allowed_hosts. Returns 0 on success. */
int agent_config_remove_host(sqlite3 *db, const char *name, const char *host);

/* T144/T196: Get current allowed_hosts for agent. Caller frees array + strings. */
char **agent_config_get_hosts(sqlite3 *db, const char *name, size_t *count);

/* T274/V120: Add tool to agent's tools whitelist. Returns 0 on success. */
int agent_config_add_tool(sqlite3 *db, const char *name, const char *tool);

#endif
