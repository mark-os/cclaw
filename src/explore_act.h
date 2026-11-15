#ifndef CCLAW_EXPLORE_ACT_H
#define CCLAW_EXPLORE_ACT_H

/* The data explorer's write side (specs/explorer.md "Actions"): named
 * actions, each validating its inputs and running the same checks as the CLI
 * verb or agent path it stands beside. There is no "edit any cell".
 *
 * Every action answers JSON: {"ok": true, "message": "...", ...} on success,
 * {"error": "<a sentence saying what was wrong and how to fix it>", ...} on a
 * refusal. Conversation history is never written here; anything the event
 * loop must carry out (an approval decision) is a queue row plus
 * wake_session(0). */

#include "sqlite3.h"

/* Run one action. body is the request's JSON object {"action": name, ...}.
 * Takes web_db_lock() itself, for database work only: the two actions that
 * call a provider over the network (provider_add, provider_edit when the
 * endpoint or key changes, provider_refresh_models) release it around the
 * call. Returns an HTTP status; *out is a malloc'd JSON body. */
int explore_act(sqlite3 *db, const char *body, char **out);

/* GET /admin/api/approval?id=N: the approval card an approver sees
 * elsewhere (approval_format_summary), plus the row's state. */
int explore_approval_card(sqlite3 *db, const char *id, char **out);

/* Where new agents' workspaces go (<dir>/<name>/workspace); NULL = none is
 * made (workspace_init creates it on first use). Set by the web server from
 * the database path, the way the daemon derives it. */
void explore_set_agents_dir(const char *dir);

#endif
