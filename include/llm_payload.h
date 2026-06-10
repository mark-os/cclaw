#ifndef CCLAW_LLM_PAYLOAD_H
#define CCLAW_LLM_PAYLOAD_H

#include "config.h"
#include "context.h"
#include "tools.h"
#include "sqlite3.h"
#include <stddef.h>

/* Payload handle — holds open SQLite statement for zero-copy access.
 * Call llm_payload_release() when done (after HTTP POST completes). */
typedef struct {
    sqlite3_stmt *stmt;   /* open statement — column 0 is the JSON payload */
    const char *body;     /* pointer into stmt result (valid until release) */
    sqlite3 *db;          /* for cleanup of temp table */
} LlmPayload;

/* Build complete LLM request payload JSON via SQLite.
 * On success: out->body points to valid JSON (zero-copy from SQLite).
 * Caller MUST call llm_payload_release() after the HTTP call completes.
 * Returns 0 on success, -1 on error. */
int llm_build_payload(sqlite3 *db, int64_t session_id, const Config *cfg,
                      const ContextPlan *plan, const char *recall_text,
                      const char *gemini_cache_name, LlmPayload *out);

/* Release payload resources (finalize stmt, drop temp table). */
void llm_payload_release(LlmPayload *p);

#endif
