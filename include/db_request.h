#ifndef CCLAW_DB_REQUEST_H
#define CCLAW_DB_REQUEST_H

#include "context.h"
#include "types.h"
#include "sqlite3.h"

/* T295: SQL-based request builder.
 * Uses json_object()/json_group_array() to produce complete LLM request
 * body directly from the entries table — no C-level JSON escaping. */

/* Build complete LLM request JSON from DB entries via SQL.
 * entry_ids: ordered array of entry IDs to include (from ContextPlan).
 * entry_count: number of entries.
 * model: model name string.
 * endpoint_type: ENDPOINT_OPENAI or ENDPOINT_GEMINI.
 * tools_json: pre-built tools JSON array string (NULL = no tools).
 * max_tokens: max output tokens (0 = omit).
 * stream: 1 = include "stream":true (OpenAI only).
 * cache_break_after: entry ID after which to add cache_control (−1 = none).
 * Returns heap-allocated JSON string. Caller frees. NULL on error. */
char *db_build_request(sqlite3 *db, int64_t session_id,
                       const int64_t *entry_ids, int entry_count,
                       const char *model, EndpointType endpoint_type,
                       const char *tools_json, int max_tokens,
                       int stream, int64_t cache_break_after);

/* Set cache_break_after on a session. Returns 0 on success. */
int session_set_cache_break(sqlite3 *db, int64_t session_id, int64_t entry_id);

/* Get cache_break_after for a session. Returns entry_id or -1. */
int64_t session_get_cache_break(sqlite3 *db, int64_t session_id);

#endif
