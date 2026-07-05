#ifndef CCLAW_CONFIG_REGISTRY_H
#define CCLAW_CONFIG_REGISTRY_H

#include <stddef.h>
#include <sqlite3.h>

/* Config registry — the single source of truth for every global config key:
 * its name, default value, and description live in one static table in C.
 * config_registry_sync() mirrors defaults+descriptions into the config table
 * at startup (code-owned columns always win); the `value` column is the
 * operator/agent override and is never touched by sync. Effective value is
 * COALESCE(value, default_value), so losing seed data breaks nothing, and
 * `SELECT key, value, default_value, description FROM config` gives an agent
 * the complete, self-describing knob inventory. */

typedef struct {
    const char *key;
    const char *def;     /* default value as text */
    const char *desc;    /* one-line human/agent-readable description */
} ConfigDef;

/* Provider fallbacks shared by the DB and env config loaders (the providers
 * table is the real source; these cover a missing/empty table). */
#define CCLAW_DEF_BASE_URL       "https://openrouter.ai/api/v1"
#define CCLAW_DEF_MODEL          "deepseek/deepseek-v4-flash"
#define CCLAW_DEF_MAX_TOKENS     4096
#define CCLAW_DEF_CONTEXT_WINDOW 128000

/* Registry lookup (no DB). Returns NULL if key is not registered. */
const char *config_default(const char *key);
int config_default_int(const char *key);
double config_default_double(const char *key);

/* Upsert every registered key's default_value + description into config.
 * Never touches `value`. Returns 0 on success. */
int config_registry_sync(sqlite3 *db);

/* Effective value: COALESCE(value, default_value) from the DB, falling back
 * to the registry default if the row is missing (pre-sync DB). Returns
 * malloc'd string, or NULL if the key is unregistered and has no row. */
char *config_get(sqlite3 *db, const char *key);
int config_get_int(sqlite3 *db, const char *key);
double config_get_double(sqlite3 *db, const char *key);

/* Set (or clear, with value == NULL) the override for a registered key.
 * Fails with -1 if the key is not in the registry — there are no anonymous
 * config writes; a key gets in only with a default and a description. */
int config_set(sqlite3 *db, const char *key, const char *value);

#endif
