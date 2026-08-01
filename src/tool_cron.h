#ifndef CCLAW_TOOL_CRON_H
#define CCLAW_TOOL_CRON_H

/* The cron_set / cron_list / cron_remove tools — an agent's interface to
 * its own scheduled work, scoped to the calling agent.
 */

#include "tools.h"
#include "sqlite3.h"
#include <stdint.h>

/* Context passed as user_data to cron tool handlers. cron_set runs inline
 * (it can park an approval), so the dispatcher refreshes session_id,
 * agent_name and current_tool_call_id before every call — one setup serves
 * whichever agent is advancing. */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    char agent_name[64];
    const char *workspace;             /* script paths resolve here */
    const char *current_tool_call_id;  /* set by dispatcher before each call */
} ToolCronCtx;

/* Register cron_set, cron_list, cron_remove tools. */
int tool_cron_register(ToolRegistry *reg, ToolCronCtx *ctx);

/* Individual handlers (exposed for testing). cron_set returns NULL when it
 * parked an approval (cross-agent targeting) — the NULL_PARK contract. */
char *tool_cron_set_handler(const char *arguments, void *user_data);
char *tool_cron_list_handler(const char *arguments, void *user_data);
char *tool_cron_remove_handler(const char *arguments, void *user_data);

#endif
