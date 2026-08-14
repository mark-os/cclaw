#ifndef CCLAW_TOOL_DB_QUERY_H
#define CCLAW_TOOL_DB_QUERY_H

/* The db_query tool: read-only SELECT access to cclaw.db for the agent,
 * returning rows as JSON. Non-SELECT statements are refused.
 */

#include "tools.h"
#include "sqlite3.h"

/* Register db_query tool. db handle used for read-only queries. */
int tool_db_query_register(ToolRegistry *reg, sqlite3 *db);

/* Handler: parse JSON args {"sql":"..."}, reject non-SELECT, return results as JSON. */
char *tool_db_query_handler(const char *arguments, void *user_data, int *is_error);

#endif
