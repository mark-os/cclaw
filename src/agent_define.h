#ifndef CCLAW_AGENT_DEFINE_H
#define CCLAW_AGENT_DEFINE_H

#include <sqlite3.h>
#include "agent_config.h"   /* GrantOrigin */

/* One agent-definition JSON schema, applied identically from all three
 * declaration paths: disk seed (agent.json), create_agent tool, and
 * extension.json agents[]. See specs/self-configuration.md.
 *
 * creator == NULL means operator (no creation caps). A non-NULL creator is
 * capped: child sandbox_profile <= creator's (host > standard > restricted
 * by looseness), child grants a subset of creator's live grants,
 * extensions published or creator-owned.
 *
 * On error returns -1 and sets *err (heap, caller frees). */

/* Parse + caps check only; no writes. */
int agent_definition_validate(sqlite3 *db, const char *json,
                              const char *creator, char **err);

/* Validate then apply: agents row (or clone_from copy + overlay), default +
 * declared grants, agent_extensions, memory blocks, workspace dir under
 * agents_dir (skipped when agents_dir is NULL). Every grant the new agent
 * gets is written with `origin` (agent_create + the approval that created
 * it, or seed for a definition shipped on disk / in a built-in manifest). */
int agent_definition_apply(sqlite3 *db, const char *json, const char *creator,
                           const char *agents_dir, GrantOrigin origin,
                           char **err);

/* Attach the extensions config agent_default_extensions names (those that
 * exist) to a newly created agent — the bundled ones, shipped in the binary
 * (extension_install_builtin), among them. An existing attachment, enabled
 * or not, is left as it is. Every path that creates an agent calls this. */
void agent_attach_default_extensions(sqlite3 *db, const char *name);

/* Update an existing agent (update_agent tool). Authorization is
 * creator-or-self via agents.created_by; caps (profile, grants) apply against
 * the caller exactly as at creation. Fields overlay (absent = keep, unknown
 * keys refused); grants are additive-only. */
int agent_definition_update_validate(sqlite3 *db, const char *json,
                                     const char *caller, char **err);
int agent_definition_update_apply(sqlite3 *db, const char *json,
                                  const char *caller, int64_t approval_id,
                                  char **err);

#endif
