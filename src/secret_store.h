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

#endif
