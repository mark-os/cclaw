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

/* Grants CRUD (delegates to agent_config) — covers all kinds ("tool",
 * "host", "read_path", "write_path"), keyed by the grants table's implicit
 * rowid so callback-data references never embed an unbounded name/value. */
typedef struct {
    int64_t id;
    char *kind;
    char *value;
} AdminGrant;

int admin_list_grants(sqlite3 *db, const char *agent_name,
                      AdminGrant **out, size_t *out_count);
void admin_grants_free(AdminGrant *list, size_t count);

/* Look up (agent_name, kind, value) by rowid, then revoke via agent_config_revoke. */
int admin_revoke_grant_by_id(sqlite3 *db, int64_t grant_id);

/* Thin wrapper over agent_config_grant(db, agent, kind, value, 0). */
int admin_grant_capability(sqlite3 *db, const char *agent_name,
                           const char *kind, const char *value);

/* Tool names for the "add tool grant" picker. */
int admin_list_tool_names(sqlite3 *db, char ***out, size_t *out_count);
void admin_tool_names_free(char **names, size_t count);

/* List providers: returns count. out_models[i] is heap-allocated (caller frees). */
typedef struct {
    int index;
    char *model;
    char *base_url;
} AdminProvider;

int admin_list_providers(sqlite3 *db, AdminProvider **out, size_t *out_count);
void admin_providers_free(AdminProvider *providers, size_t count);

/* Model routing candidates in priority order (first healthy wins), joined
 * with provider metadata and key presence — drives the /model command. */
typedef struct {
    char *id;              /* models.id */
    char *model;           /* provider-facing model string */
    char *provider;        /* provider name */
    char *base_url;
    char *api_key_env;     /* env var / secret name the provider resolves */
    char *status;          /* healthy | degraded | disabled */
    int has_key;           /* key present in env or encrypted kv */
    int context_window;    /* 0 = global default */
    int degraded_left;     /* seconds until degradation cooldown expires, 0 if past/none */
    int64_t total_requests;
    int64_t err_5xx;
    int64_t err_429;
} AdminModel;

int admin_list_models(sqlite3 *db, AdminModel **out, size_t *out_count);
void admin_models_free(AdminModel *list, size_t count);

/* Make model_id the head of the routing order. The previous head shifts to
 * first fallback (llm_req's per-request skip + degradation give automatic
 * fallback); the chosen model's health is reset so stale degradation can't
 * sideline an explicit operator switch. Agents whose model preference pointed
 * at the previous head (or was unset) are repointed so context-window
 * resolution follows. Writes the previous head's id into prev (empty if none).
 * Returns 0 on success, -1 if model_id doesn't exist. */
int admin_switch_model(sqlite3 *db, const char *model_id, char *prev, size_t prev_sz);

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
