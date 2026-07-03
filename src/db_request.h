#ifndef CCLAW_DB_REQUEST_H
#define CCLAW_DB_REQUEST_H

#include "context.h"
#include "types.h"
#include "sqlite3.h"

/* T295: SQL-based request builder.
 * Uses json_object()/json_group_array() to produce complete LLM request
 * body directly from the entries table — no C-level JSON escaping. */

/* Build messages JSON array from flat typed entries (new event-sourced format).
 * Returns just the messages/contents JSON — caller wraps with model/tools.
 * replay_reasoning: 1 = include reasoning as reasoning_content on assistant msgs.
 * Returns heap-allocated JSON string. Caller frees. NULL on error. */
char *db_build_request_typed(sqlite3 *db, int64_t session_id,
                             const int64_t *entry_ids, int entry_count,
                             EndpointType endpoint_type,
                             int replay_reasoning);

#endif
