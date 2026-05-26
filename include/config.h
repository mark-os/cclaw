#ifndef CCLAW_CONFIG_H
#define CCLAW_CONFIG_H

#include "types.h"
#include "sqlite3.h"

/* V61,T170: Load config from kv table + env var overrides.
 * Priority: env var > kv value > hardcoded default.
 * Returns heap-allocated Config, or NULL on failure. */
Config *config_load_from_kv(sqlite3 *db);

/* T46: Render system prompt with template vars {session_id}, {date}.
 * Returns heap-allocated string. Caller must free. */
char *config_render_system_prompt(const Config *cfg, int64_t session_id);

/* Ensure workspace directory exists and populate SOUL.md/MEMORY.md on first use.
 * Returns 0 on success, -1 on error. */
int workspace_init(const Config *cfg);

/* Free config and all owned strings. */
void config_free(Config *cfg);

#endif
