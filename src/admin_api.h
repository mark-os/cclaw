#ifndef CCLAW_ADMIN_API_H
#define CCLAW_ADMIN_API_H

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

/* Generic admin operations — channel-agnostic, usable by CLI or any channel. */

/* Store an API key in the encrypted kv (never a file). Returns 0 on success.
 * provider: "openrouter", "gemini", or NULL/"custom" (var_name=value format).
 * For known providers, the kv key is the canonical env var name, so the
 * config loader's env → encrypted-kv fallback finds it. */
int admin_set_key(sqlite3 *db, const char *provider, const char *value);

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

/* Approvals visible to a channel's admin(s) — scoped to sessions routed to
 * that channel (see handle_approval_park's admin routing). */
typedef struct {
    int64_t id;
    int64_t session_id;
    char *agent_name;
    char *tool_name;
    char *action;
    char *args_json;
} AdminApproval;

/* Currently pending approvals for sessions on this channel. */
int admin_list_pending_approvals(sqlite3 *db, const char *channel_name,
                                 AdminApproval **out, size_t *out_count);
/* Most recently denied approvals for sessions on this channel (newest first,
 * capped at limit). */
int admin_list_denied_approvals(sqlite3 *db, const char *channel_name, int limit,
                                AdminApproval **out, size_t *out_count);
void admin_approvals_free(AdminApproval *list, size_t count);

/* Apply a grant directly from a past approval's action/args (request_config's
 * grant_tool/grant_host/grant_path only — a rename isn't a standing
 * capability grant, so it's not eligible here). No agent involvement, no new
 * pending approval created. Records a fresh 'approved' row for audit trail;
 * the looked-up approval row itself is left untouched. Returns 0 on success,
 * -1 if the approval doesn't exist or isn't a grantable action. */
int admin_grant_from_history(sqlite3 *db, int64_t approval_id);

#endif
