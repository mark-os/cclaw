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

/* Resolve *and create* the agent's scratch directory — bind-mounted as /tmp in
 * the sandboxed child. Lives on the host's own /tmp (or `root_override`, from
 * the `tmp_root` config key) so it inherits whatever the host does for temp
 * storage — tmpfs or disk, and the host's tmpfiles.d cleaner — instead of this
 * code inventing a size policy. Persistent: a build's intermediates must
 * survive from one tool call to the next.
 *
 * Layout is <root>/cclaw-<uid>/<agent>/. The uid is there so two users running
 * cclaw on one box do not collide on a 0700 directory (this is not what stops
 * squatting — the ownership check below is).
 *
 * /tmp is world-writable, so every level this creates is 0700 and verified: on
 * EEXIST the path must be a real directory, owned by us, mode 0700, not a
 * symlink, or the call fails. Returns `out` on success, NULL on any failure —
 * callers treat NULL as "no scratch bind", never as a reason to fall back to
 * something wider.
 *
 * Also opportunistically reaps abandoned namespace roots under
 * <root>/cclaw-<uid>/.ns/ (a live one holds a mount and refuses rmdir, a dead
 * one is empty and goes), so nothing has to track child pids to avoid a leak. */
const char *scratch_dir_ensure(const char *agent, const char *root_override,
                               char *out, size_t cap);

/* Free config and all owned strings. */
void config_free(Config *cfg);

#endif
