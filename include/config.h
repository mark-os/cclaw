#ifndef CCLAW_CONFIG_H
#define CCLAW_CONFIG_H

#include "types.h"
#include "sqlite3.h"

/* Load config for parent processes (CLI/daemon).
 * Priority: CCLAW_* env vars > cclaw.db kv > hardcoded defaults.
 * Returns heap-allocated Config, or NULL on failure. */
Config *config_load(sqlite3 *cclaw_db);

/* Load config for agent processes (forked children).
 * Reads only CCLAW_* env vars + hardcoded defaults.
 * No DB reads — parent injects everything at fork. */
Config *config_load_from_env(void);

/* Render system prompt with template vars {session_id}, {date}, {workspace}.
 * Returns heap-allocated string. Caller must free. */
char *config_render_system_prompt(const Config *cfg, int64_t session_id);

/* Ensure workspace directory exists. Returns 0 on success, -1 on error. */
int workspace_init(const Config *cfg);

/* Free config and all owned strings. */
void config_free(Config *cfg);

#endif
