#ifndef CCLAW_AGENT_DEFINE_H
#define CCLAW_AGENT_DEFINE_H

#include <sqlite3.h>

/* One agent-definition JSON schema, applied identically from all three
 * declaration paths: disk seed (agent.json), create_agent tool, and
 * extension.json agents[]. See specs/self-configuration.md.
 *
 * creator == NULL means operator (no creation caps). A non-NULL creator is
 * capped: child sandbox_profile <= creator's (host > trusted > standard >
 * restricted by looseness), child grants a subset of creator's live grants,
 * extensions published or creator-owned.
 *
 * On error returns -1 and sets *err (heap, caller frees). */

/* Parse + caps check only; no writes. */
int agent_definition_validate(sqlite3 *db, const char *json,
                              const char *creator, char **err);

/* Validate then apply: agents row (or clone_from copy + overlay), default +
 * declared grants, agent_extensions, memory blocks, workspace dir under
 * agents_dir (skipped when agents_dir is NULL). */
int agent_definition_apply(sqlite3 *db, const char *json, const char *creator,
                           const char *agents_dir, char **err);

/* Update an existing agent (update_agent tool). Authorization is
 * creator-or-self via agents.created_by; caps (profile, grants) apply against
 * the caller exactly as at creation. Fields overlay (absent = keep, unknown
 * keys refused); grants are additive-only. */
int agent_definition_update_validate(sqlite3 *db, const char *json,
                                     const char *caller, char **err);
int agent_definition_update_apply(sqlite3 *db, const char *json,
                                  const char *caller, char **err);

#endif
