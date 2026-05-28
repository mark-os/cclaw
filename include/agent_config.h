#ifndef CCLAW_AGENT_CONFIG_H
#define CCLAW_AGENT_CONFIG_H

#include <stddef.h>
#include <sqlite3.h>
#include "types.h"

/* T75: agent discovery — scan agents/ dir, list available agents by name.
 * Returns heap-allocated array of agent names (each heap-allocated).
 * Caller must free each name and the array. Sets *count. */
char **agent_discover(const char *agents_dir, size_t *count);

/* Free array returned by agent_discover. */
void agent_discover_free(char **names, size_t count);

/* T76: per-agent config loaded from agents/<name>/agent.json.
 * Fields override global Config when non-NULL/non-zero. */
typedef struct {
    char *name;             /* agent name (from dir) */
    char *model;            /* model override (NULL = use global) */
    char *workspace;        /* V12: per-agent workspace (NULL = ./workspace/{name}) */
    char **tools;           /* tool whitelist (NULL = all tools) */
    size_t tool_count;
    char **allowed_hosts;   /* V38: hostnames for http_fetch */
    size_t allowed_hosts_count;
    char **read_access;     /* V66: dirs granted read-only in landlock */
    size_t read_access_count;
    int max_iterations;     /* 0 = use global */
} AgentConfig;

/* Load agent config from agents_dir/name/agent.json (legacy, for migration).
 * Returns NULL on missing file (caller should use global defaults).
 * Applies V12 workspace fallback if workspace not specified. */
AgentConfig *agent_config_load(const char *agents_dir, const char *name);

/* T196/V80: Load agent config from daemon.db agent_config table.
 * Returns NULL if no config rows exist for this agent.
 * Applies V12 workspace fallback if workspace not specified. */
AgentConfig *agent_config_load_db(sqlite3 *db, const char *name);

/* T196/V80: Save agent config to daemon.db agent_config table.
 * Upserts all non-NULL fields as key-value rows. Returns 0 on success. */
int agent_config_save_db(sqlite3 *db, const AgentConfig *ac);

/* T196: Import agent.json into daemon.db agent_config table, delete file after.
 * Called during daemon startup migration. Returns 0 on success, -1 on error. */
int agent_config_migrate_json(sqlite3 *db, const char *agents_dir, const char *name);

/* Free AgentConfig returned by agent_config_load. */
void agent_config_free(AgentConfig *ac);

/* Merge agent config into a copy of global config.
 * Returns heap-allocated Config with agent overrides applied.
 * Caller must config_free() the result. ac may be NULL (returns copy of global). */
Config *agent_config_merge(const Config *global, const AgentConfig *ac);

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
 * Seeds agent from disk if not in DB. Renders template vars {session_id}, {date}, {agent_name}.
 * Falls back to config_render_system_prompt if agent_name is NULL.
 * Returns heap-allocated string. Caller must free. */
char *agent_build_system_prompt(sqlite3 *db, const char *agent_name,
                                int64_t session_id, const char *agents_dir,
                                const Config *fallback_cfg);

/* T150/T196: Create a new agent from approval payload.
 * Creates agents_dir/name/ directory + workspace, writes config to daemon.db agent_config,
 * seeds agents table + system.md on disk.
 * payload is a cJSON object with: name, model, system_prompt, tools[], allowed_hosts[].
 * Returns 0 on success, -1 on failure. */
int agent_config_create(const char *agents_dir, sqlite3 *db, const char *payload_json);

/* T186: Create an ephemeral agent directory with agent.db + workspace.
 * Generates name "ephemeral-<uuid>". Creates agents_dir/name/ with agent.json,
 * agent.db (initialized), and workspace/ subdir. Seeds DB row.
 * Returns heap-allocated agent name on success (caller frees), NULL on failure. */
char *agent_create_ephemeral(const char *agents_dir, sqlite3 *db);

/* T144/T196: Add host to agent's allowed_hosts in daemon.db agent_config.
 * Returns 0 on success, -1 on failure. */
int agent_config_add_host(sqlite3 *db, const char *name, const char *host);

/* T144/T196: Remove host from agent's allowed_hosts in daemon.db agent_config.
 * Returns 0 on success (including host not found), -1 on failure. */
int agent_config_remove_host(sqlite3 *db, const char *name, const char *host);

/* T144/T196: Get current allowed_hosts for agent from daemon.db. Returns heap-allocated array.
 * Caller must free each string and the array. Sets *count. */
char **agent_config_get_hosts(sqlite3 *db, const char *name, size_t *count);

#endif
