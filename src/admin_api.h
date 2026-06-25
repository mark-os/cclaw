#ifndef CCLAW_ADMIN_API_H
#define CCLAW_ADMIN_API_H

#include <sqlite3.h>
#include <stddef.h>

/* Generic admin operations — channel-agnostic, usable by CLI or any channel. */

/* Write an API key to the env file. Returns 0 on success.
 * provider: "openrouter", "gemini", or NULL for custom (var_name=value format).
 * For known providers, maps to the canonical env var name. */
int admin_set_key(const char *env_file, const char *provider, const char *value);

/* Map provider name to env var. Returns NULL if unknown. */
const char *admin_key_env_name(const char *provider);

/* Set model for provider at index (0=primary, 1+=fallback). Returns 0 on success. */
int admin_set_model(sqlite3 *db, int provider_index, const char *model);

/* Set base_url for provider at index. Returns 0 on success. */
int admin_set_endpoint(sqlite3 *db, int provider_index, const char *url);

/* Host whitelist management (delegates to agent_config). */
int admin_add_host(sqlite3 *db, const char *agent_name, const char *host);
int admin_remove_host(sqlite3 *db, const char *agent_name, const char *host);

/* List providers: returns count. out_models[i] is heap-allocated (caller frees). */
typedef struct {
    int index;
    char *model;
    char *base_url;
} AdminProvider;

int admin_list_providers(sqlite3 *db, AdminProvider **out, size_t *out_count);
void admin_providers_free(AdminProvider *providers, size_t count);

/* List agents. Returns heap-allocated array of names (caller frees each + array). */

/* Write key=value to env file, replacing existing line if present. Returns 0 on success. */
int admin_write_env_key(const char *env_file, const char *var_name, const char *value);

#endif
