#ifndef CCLAW_CONTEXT_H
#define CCLAW_CONTEXT_H

#include "types.h"
#include "sqlite3.h"
#include <stddef.h>

/* Lightweight entry metadata for streaming request planning (pass 1).
 * No content loaded — just enough to determine cut point. */
typedef struct {
    int64_t id;
    Role role;
    StopReason stop_reason;
    int token_estimate;     /* chars/4 heuristic from length(data) */
    int tool_call_count;    /* number of tool_calls in assistant msg (0 otherwise) */
    char include;           /* in the request: past the cut, or pinned (data.pin) */
} PlanEntry;

/* Result of context_plan() — ordered entry IDs + cut point. */
typedef struct {
    PlanEntry *entries;     /* heap-allocated array */
    int count;              /* total entries after filtering */
    int cut;               /* index of first included entry (skip entries[0..cut-1]) */
    int budget;            /* token budget used for planning */
} ContextPlan;

/* Plan which entries to include in LLM request (pass 1).
 * Queries SQLite for branch metadata without loading content.
 * overhead: estimated tokens consumed by tool definitions + other fixed content.
 * context_window: effective context window for this request (0 = use cfg default).
 * Returns 0 on success, -1 on error. Caller frees with context_plan_free(). */
int context_plan(sqlite3 *db, int64_t session_id, const Config *cfg, int overhead, ContextPlan *out);

/* Free a ContextPlan. */
void context_plan_free(ContextPlan *plan);

/* Truncate tool result at write time. If content exceeds limits, writes
 * full output to /tmp/cclaw-<session_id>/<tool_call_id>.out and returns
 * truncated content with file path reference. Returns malloc'd string (caller frees). */
char *truncate_and_spill(const char *src, int64_t session_id, const char *tool_call_id);

/* Build session temp dir path: /tmp/cclaw-<session_id> */
void session_tmp_dir(int64_t session_id, char *buf, size_t bufsz);

/* Auto-recall — FTS5 search across sessions for relevant context.
 * Returns heap-allocated text (caller frees) or NULL if nothing recalled. */
/* Compaction trigger: branch token sum exceeds context_threshold x window. */
int session_needs_compaction(sqlite3 *db, int64_t session_id, const Config *cfg);

/* Resolve the effective context window for an agent's model.
 * Priority: models.context_window (non-NULL) → global_default.
 * Returns global_default if agent has no model or model has NULL window. */
int model_context_window(sqlite3 *db, const char *agent_name, int global_default);

char *context_auto_recall(sqlite3 *db, int64_t session_id, const char *user_msg,
                          int max_tokens);

#endif
