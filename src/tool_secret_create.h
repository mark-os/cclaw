#ifndef CCLAW_TOOL_SECRET_CREATE_H
#define CCLAW_TOOL_SECRET_CREATE_H

#include "tools.h"
#include "sqlite3.h"

/* Context for secret_create (inline in parent process — needs the db handle
 * to write the secrets table; the master key lives process-wide via
 * db_set_secret_key, checked at call time). */
typedef struct {
    sqlite3 *db;
    char agent_name[64];   /* the advancing agent, set per call by dispatch */
} ToolSecretCreateCtx;

/* Register secret_create: mints a random credential, stores it encrypted in
 * the DB (source='generated'), and returns only its {{SECRET:name}}
 * placeholder — the value itself never enters the model's context. The
 * minting agent is granted it (source 'capture') in the same transaction; no
 * other agent may use it until granted. It has zero secret_hosts rows, so
 * it reaches no host until a binding is approved. */
int tool_secret_create_register(ToolRegistry *reg, ToolSecretCreateCtx *ctx);

#endif
