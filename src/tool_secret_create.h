#ifndef CCLAW_TOOL_SECRET_CREATE_H
#define CCLAW_TOOL_SECRET_CREATE_H

#include "tools.h"
#include "sqlite3.h"

/* Context for secret_create (inline in parent process — needs the db handle
 * to write the secrets table; the master key lives process-wide via
 * db_set_secret_key, checked at call time). */
typedef struct {
    sqlite3 *db;
} ToolSecretCreateCtx;

/* Register secret_create: mints a random credential, stores it encrypted in
 * the DB (source='generated'), and returns only its {{SECRET:name}}
 * placeholder — the value itself never enters the model's context. Composes
 * with the fail-closed binding rule for free: the new secret has zero
 * secret_hosts rows, so its first use always parks. */
int tool_secret_create_register(ToolRegistry *reg, ToolSecretCreateCtx *ctx);

#endif
