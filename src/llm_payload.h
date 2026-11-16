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
    sqlite3 *db;          /* for cleanup of the temp tables */
} LlmPayload;

/* Build complete LLM request payload JSON via SQLite.
 * user_context (the user context block: clock, recall, live state) rides as
 * its own user message right before the first entry of the newest turn — see
 * SQL_TRANSCRIPT_CONTEXT in llm_payload.c for the placement rule.
 * On success: out->body points to valid JSON (zero-copy from SQLite).
 * Caller MUST call llm_payload_release() after the HTTP call completes.
 * Returns 0 on success, -1 on error. */
int llm_build_payload(sqlite3 *db, int64_t session_id, const Config *cfg,
                      const ContextPlan *plan, const char *user_context,
                      const char *system_prompt, LlmPayload *out);

/* Builds the user context block (the full <RELEVANT_CONTEXT> text): the local
 * wall clock with its timezone, recall_text (computed by the caller via
 * context_auto_recall — the one piece not natively SQL), plus
 * context-placement memory blocks, running sub-agents, background jobs and
 * pending approvals, each queried live. Pass recall_text (or NULL) in; the
 * result is ready to pass straight through as llm_build_payload's
 * user_context. Always non-NULL (the clock is unconditional); NULL only on a
 * query failure. Caller frees. */
char *user_context_text(sqlite3 *db, int64_t session_id, const char *recall_text);

/* Release payload resources (finalize stmt, drop temp tables). */
void llm_payload_release(LlmPayload *p);

/* The JSON object to merge into the request body for this route's
 * reasoning-effort level (models.effort_map spelling), or NULL when nothing
 * should be sent. Caller frees. Shared with the js-tier llm() wire builder. */
char *llm_effort_fragment(sqlite3 *db, const ProviderConfig *prov);

#endif
