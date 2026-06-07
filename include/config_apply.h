#ifndef CCLAW_CONFIG_APPLY_H
#define CCLAW_CONFIG_APPLY_H

#include <sqlite3.h>

/* Handle config tool calls (configure_provider, configure_channel, create_agent,
 * rename_agent, request_config). Returns malloc'd result string. */
char *config_apply(sqlite3 *db, const char *agent_name,
                   const char *tool_name, const char *args_json);

#endif
