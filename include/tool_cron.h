#ifndef CCLAW_TOOL_CRON_H
#define CCLAW_TOOL_CRON_H

#include "tools.h"
#include "sqlite3.h"
#include <stdint.h>

/* Context passed as user_data to cron tool handlers */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
} ToolCronCtx;

/* Register cron_set, cron_list, cron_remove tools. */
int tool_cron_register(ToolRegistry *reg, ToolCronCtx *ctx);

/* Individual handlers (exposed for testing) */
char *tool_cron_set_handler(const char *arguments, void *user_data);
char *tool_cron_list_handler(const char *arguments, void *user_data);
char *tool_cron_remove_handler(const char *arguments, void *user_data);

#endif
