#ifndef CCLAW_DB_RESPONSE_H
#define CCLAW_DB_RESPONSE_H

#include "types.h"
#include "sqlite3.h"
#include <stdint.h>

/* T296: SQL response ingress — write LLM response into entries + tool_calls tables.
 *
 * Non-streaming: json_extract() parses response JSON directly in SQL.
 * Streaming: accumulated fields written via parameterized INSERT.
 * Tool call arguments validated with json() before storage. */

/* Ingested response result */
typedef struct {
    int64_t entry_id;       /* inserted entries row */
    int tool_call_count;    /* number of tool_calls rows inserted */
} IngestResult;

/* Ingest a non-streaming LLM response JSON directly into DB.
 * Uses json_extract() to pull fields from the raw response body.
 * Validates tool_call arguments with json() — invalid JSON stored as-is with warning.
 * Returns 0 on success, -1 on error. */
int db_ingest_response(sqlite3 *db, int64_t session_id, int64_t parent_id,
                       int64_t turn_id, const char *model,
                       const char *response_json, IngestResult *out);

/* Ingest accumulated streaming response fields into DB.
 * content/reasoning may be NULL. tool_calls passed as parallel arrays.
 * Returns 0 on success, -1 on error. */
int db_ingest_streaming(sqlite3 *db, int64_t session_id, int64_t parent_id,
                        int64_t turn_id, const char *model,
                        const char *content, const char *finish_reason,
                        int usage_in, int usage_out, int64_t cost_nano,
                        const char *const *tc_ids, const char *const *tc_names,
                        const char *const *tc_args, int tc_count,
                        IngestResult *out);

/* Mark a tool_call row as completed (status = 'done'). */
int db_tool_call_complete(sqlite3 *db, int64_t entry_id, const char *call_id);

#endif
