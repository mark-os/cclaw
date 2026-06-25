#ifndef CCLAW_DB_RESPONSE_H
#define CCLAW_DB_RESPONSE_H

#include "types.h"
#include "sqlite3.h"
#include <stdint.h>

/* T296: SQL response ingress — write LLM response into entries + tool_calls tables.
 * Tool call arguments validated with json() before storage. */

/* Ingested response result */
typedef struct {
    int64_t entry_id;       /* inserted entries row */
    int tool_call_count;    /* number of tool_calls rows inserted */
} IngestResult;

/* Mark a tool_call row as completed (status = 'done'). */
int db_tool_call_complete(sqlite3 *db, int64_t entry_id, const char *call_id);

/* Mark a tool_call row as completed and record the result entry. */
int db_tool_call_complete_with_result(sqlite3 *db, int64_t entry_id,
                                      const char *call_id, int64_t result_entry_id);

/* Typed response result */
typedef struct {
    int64_t assistant_entry_id; /* the assistant_message entry */
    int64_t *tc_entry_ids;      /* heap array of tool_call entry IDs (caller frees) */
    int tc_count;
} TypedIngestResult;

/* Ingest streaming response as flat typed entries.
 * Creates: reasoning (optional) + assistant_message + N tool_call entries.
 * All share the same turn_id. Returns 0 on success, -1 on error. */
int db_ingest_typed(sqlite3 *db, int64_t session_id, int64_t turn_id,
                    const char *model, const char *content, const char *reasoning,
                    const char *finish_reason,
                    int usage_in, int usage_out, int64_t cost_nano,
                    const char *const *tc_ids, const char *const *tc_names,
                    const char *const *tc_args, int tc_count,
                    TypedIngestResult *out);

#endif
