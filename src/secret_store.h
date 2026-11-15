#ifndef CCLAW_SECRET_STORE_H
#define CCLAW_SECRET_STORE_H

#include "types.h"   /* ShellSecret */
#include <sqlite3.h>
#include <stddef.h>

/* Per-call secret snapshot (specs/security.md): merges the process's
 * env-collected secrets (immutable base, borrowed — never mutated) with the
 * DB-backed store, read fresh on every call so a secret born mid-session
 * (operator `secret set`, secret_create) becomes usable on its very next
 * dispatch. env_base wins on a name collision. Deep-copies everything, so
 * the result is safe to free independently of env_base's lifetime.
 * Returns NULL with *out_count=0 if both sources are empty. */
ShellSecret *secrets_snapshot(sqlite3 *db, const ShellSecret *env_base,
                              size_t env_count, size_t *out_count);

/* Free a snapshot from secrets_snapshot: wipes values, frees names+array. */
void secrets_snapshot_free(ShellSecret *secrets, size_t count);

/* Using a secret is a grant (specs/security.md): an agent may have a secret
 * interpolated into, or delivered to, a tool call only if it holds a live
 * grants row (kind 'secret', value = the name). Environment and stored
 * secrets alike — the rule is by name.
 * secret_granted: 1 if `agent` holds that grant.
 * secrets_granted: the subset of `all` the agent holds, as a new array whose
 * entries BORROW all's strings — free the array alone (free()), never its
 * items, and never use it after `all` is freed. NULL/0 when none.
 * Masking is not gated: output is always masked with every known value, so
 * a secret an agent may not use is still never shown to it. */
int secret_granted(sqlite3 *db, const char *agent, const char *name);
ShellSecret *secrets_granted(sqlite3 *db, const char *agent,
                             const ShellSecret *all, size_t n, size_t *out_n);

/* Scan/redact + UTF-8-sanitize an external-origin result (sub-agent final
 * text, background job output) before it enters another session's context.
 * Returns a new string (caller frees), or NULL for NULL input. */
char *tool_result_scrub(sqlite3 *db, const char *text);

#endif
