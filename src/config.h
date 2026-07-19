#ifndef CCLAW_CONFIG_H
#define CCLAW_CONFIG_H

#include "types.h"
#include "sqlite3.h"

/* Load config for parent processes (CLI/daemon).
 * Priority: CCLAW_* env vars > cclaw.db kv > hardcoded defaults.
 * Returns heap-allocated Config, or NULL on failure. */
Config *config_load(sqlite3 *cclaw_db);

/* Render system prompt with template vars {session_id}, {date}, {workspace}.
 * Returns heap-allocated string. Caller must free. */
char *config_render_system_prompt(const Config *cfg, int64_t session_id);

/* Ensure workspace directory exists. Returns 0 on success, -1 on error. */
int workspace_init(const Config *cfg);

/* Resolve the agent folder — where per-agent runtime artifacts (e.g. the egress
 * proxy socket) live, distinct from the agent-visible workspace. Prefers the
 * parent of `workspace`; with no workspace, anchors to the DB directory's
 * agents/ tree (<db_dir>/agents); last resort ".cclaw/agents". Writes into
 * `out` (size `cap`) and returns it. Pure — does not create the directory. */
const char *agent_dir_resolve(const char *workspace, const char *db_path,
                              char *out, size_t cap);

/* Free config and all owned strings. */
void config_free(Config *cfg);

#endif
