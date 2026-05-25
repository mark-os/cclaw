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
    int max_iterations;     /* 0 = use global */
    int shell_network;      /* V37: if true, skip CLONE_NEWNET for shell_exec */
} AgentConfig;

/* Load agent config from agents_dir/name/agent.json.
 * Returns NULL on missing file (caller should use global defaults).
 * Applies V12 workspace fallback if workspace not specified. */
AgentConfig *agent_config_load(const char *agents_dir, const char *name);

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

#endif
