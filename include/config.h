#ifndef CCLAW_CONFIG_H
#define CCLAW_CONFIG_H

#include "types.h"

/* Load config from JSON file (NULL = env-only). Env vars override JSON fields.
 * Returns heap-allocated Config, or NULL on failure. */
Config *config_load(const char *path);

/* T46: Render system prompt with template vars {session_id}, {date}.
 * Returns heap-allocated string. Caller must free. */
char *config_render_system_prompt(const Config *cfg, int64_t session_id);

/* Free config and all owned strings. */
void config_free(Config *cfg);

#endif
