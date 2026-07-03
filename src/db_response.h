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

/* Mark a tool_call row as completed and record the result entry. */
int db_tool_call_complete_with_result(sqlite3 *db, int64_t entry_id,
                                      const char *call_id, int64_t result_entry_id);

/* Status of ingesting an LLM response body. */
typedef enum {
    LLM_RESP_OK = 0,     /* well-formed; entries written */
    LLM_RESP_EMPTY,      /* zero-usage empty-stop provider glitch — retry next model */
    LLM_RESP_MALFORMED,  /* missing/empty choices|candidates — treat as failure */
    LLM_RESP_DBERR       /* our-side DB error (prepare failed) — body is valid, not a provider issue */
} LlmRespStatus;

/* Usage scalars from a successful ingest (for model stats). */
typedef struct {
    int64_t assistant_entry_id; /* the assistant_message entry */
    int prompt_tokens;
    int completion_tokens;
    int64_t cost_nano;
} TypedIngestResult;

/* Parse an LLM response body (OpenAI or Gemini, selected by ep) straight into
 * entries + tool_calls rows. Zero-copy: the JSON body is bound once and the
 * extracted column pointers bind directly into the inserts — no intermediate
 * heap copies. Creates: reasoning (optional) + assistant_message + N tool_call
 * entries, all sharing turn_id. On LLM_RESP_OK, *out carries usage/cost. */
LlmRespStatus db_ingest_response(sqlite3 *db, int64_t session_id, int64_t turn_id,
                                 const char *model, EndpointType ep,
                                 const char *body, TypedIngestResult *out);

/* Archive a raw response body for forensics regardless of HTTP outcome. Parses
 * to JSONB when valid, stores raw text otherwise. status is a free-form label
 * (e.g. "http_500", "timeout"). Retention via config 'llm_response_archive_max'
 * (default 500): >0 keeps the most recent N, 0 disables archiving, <0 keeps all.
 * db_ingest_response archives the 2xx bodies it handles; this is for the
 * non-2xx / network-error paths. request_body (the payload we sent, may be NULL)
 * is stored as JSONB alongside the failure so the request that triggered it is
 * recoverable for troubleshooting. */
void db_archive_response(sqlite3 *db, int64_t session_id, int64_t turn_id,
                         const char *model, const char *status, const char *body,
                         const char *request_body);

#endif
