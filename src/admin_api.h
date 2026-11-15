#ifndef CCLAW_ADMIN_API_H
#define CCLAW_ADMIN_API_H

#include <sqlite3.h>
#include <stddef.h>
#include <stdint.h>

/* Operator operations shared by more than one surface: the data explorer's
 * actions (explore_act.c) and the channel runners' admin commands
 * (qjs_host_channel.c). */

/* Store an API key in the encrypted secret store (never a file). Returns 0 on
 * success. provider: any providers-table name, or NULL/"custom"
 * (var_name=value format). For a known provider the secret's name is its
 * api_key_env (named <PROVIDER>_API_KEY first when blank), scope system, so
 * the request path's env → secret fallback finds it. */
int admin_set_key(sqlite3 *db, const char *provider, const char *value);

/* Map provider name to its api_key_env from the providers table.
 * Returns a malloc'd string (caller frees), NULL if unknown/keyless. */
char *admin_key_env_name(sqlite3 *db, const char *provider);

/* Approvals visible to a channel's admin(s) — scoped to sessions routed to
 * that channel (see handle_approval_park's admin routing). */
typedef struct {
    int64_t id;
    int64_t session_id;
    char *agent_name;
    char *tool_name;
    char *park_reason;
    char *args_json;
} AdminApproval;

/* Currently pending approvals for sessions on this channel
 * (channel_name NULL = all channels). */
int admin_list_pending_approvals(sqlite3 *db, const char *channel_name,
                                 AdminApproval **out, size_t *out_count);
void admin_approvals_free(AdminApproval *list, size_t count);

/* Apply a grant directly from a past approval's action/args (request_config's
 * request_changes document only — a rename isn't a standing capability
 * grant, so it's not eligible here). No agent involvement, no new pending
 * approval created. Records a fresh 'approved' row (decided_via names who
 * decided: "dashboard", "cli", ...) that the grants it applies point at; the
 * looked-up approval row itself is left untouched. Returns 0 on success,
 * -1 if the approval doesn't exist or isn't a grantable action. */
int admin_grant_from_history(sqlite3 *db, int64_t approval_id, const char *decided_via);

#endif
