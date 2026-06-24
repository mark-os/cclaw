#ifndef CCLAW_AGENT_CONFIG_H
#define CCLAW_AGENT_CONFIG_H

#include <stddef.h>
#include <stdint.h>
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

/* Live-refreshable capability arrays from grants table. */
typedef struct {
    char **hosts;
    size_t host_count;
    char **tools;
    size_t tool_count;
    char **read_paths;
    size_t read_count;
    char **write_paths;
    size_t write_count;
} AgentCaps;

/* Load all grants for an agent into caps (zeroes struct first). */
void agent_caps_load(sqlite3 *db, const char *agent, AgentCaps *caps);

/* Free existing arrays, reload in place. */
void agent_caps_refresh(sqlite3 *db, const char *agent, AgentCaps *caps);

/* Free all arrays in caps. */
void agent_caps_free(AgentCaps *caps);

/* Load agent config from cclaw.db agents table.
 * Returns NULL if agent not found. */
AgentConfig *agent_config_load_db(sqlite3 *db, const char *name);

/* Free AgentConfig. */
void agent_config_free(AgentConfig *ac);

/* Uniform grant/revoke API over the grants table. */
int agent_config_grant(sqlite3 *db, const char *agent, const char *kind,
                       const char *value, int64_t expires_at);
int agent_config_revoke(sqlite3 *db, const char *agent, const char *kind,
                        const char *value);

/* True if a live (unexpired) grant exists for (agent, kind, value). This is
 * the authorization check the dispatch gate uses: a tool with no grant is
 * denied, regardless of agent_tool_mode's run-freely default. */
int grants_contains(sqlite3 *db, const char *agent, const char *kind,
                    const char *value);

/* Seed an agent's baseline tool grants (the safe default toolset; excludes
 * shell_exec/web_fetch/db_query, which require an explicit request_config
 * grant). Idempotent — uses INSERT OR IGNORE per tool. */
void agent_grant_defaults(sqlite3 *db, const char *agent);

/* §4 Axis B — per-(agent,tool) approval mode. */
typedef enum {
    TOOL_MODE_SILENT,   /* runs freely */
    TOOL_MODE_ALWAYS,   /* every call parks for approval */
    TOOL_MODE_DECIDES   /* predicate decides; fail-closed to ALWAYS if none */
} ToolApprovalMode;

/* Approval mode of a granted tool. Ungranted/unknown → TOOL_MODE_SILENT
 * (uninstrumented tools keep their run-freely default). */
ToolApprovalMode agent_tool_mode(sqlite3 *db, const char *agent, const char *tool);

/* Set approval_mode on an existing tool grant. mode is one of
 * "silent"/"always"/"tool_decides". Returns 0 on success (a row was updated),
 * -1 on error or if the tool is not granted. */
int agent_config_set_tool_mode(sqlite3 *db, const char *agent,
                               const char *tool, const char *mode);

/* Returns malloc'd JSON array string (e.g. '["a","b"]'). Caller frees. */
char *grants_json(sqlite3 *db, const char *agent, const char *kind);

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

#endif
