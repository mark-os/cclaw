#ifndef CCLAW_CONFIG_H
#define CCLAW_CONFIG_H

#include "types.h"

/* Load config from JSON file (NULL = env-only). Env vars override JSON fields.
 * Returns heap-allocated Config, or NULL on failure. */
Config *config_load(const char *path);

/* T46: Render system prompt with template vars {session_id}, {date}.
 * Returns heap-allocated string. Caller must free. */
char *config_render_system_prompt(const Config *cfg, int64_t session_id);

/* Ensure workspace directory exists and populate SOUL.md/MEMORY.md on first use.
 * Returns 0 on success, -1 on error. */
int workspace_init(const Config *cfg);

/* Free config and all owned strings. */
void config_free(Config *cfg);

/* T142: Update model name for provider at index in config JSON file.
 * index 0 = primary provider, 1+ = fallback providers[index-1].
 * Returns 0 on success, -1 on failure. */
int config_update_model(const char *config_path, int provider_index, const char *model);

#endif
