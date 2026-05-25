#ifndef CCLAW_CONTEXT_H
#define CCLAW_CONTEXT_H

#include "types.h"
#include "sqlite3.h"
#include <stddef.h>

/* V41: Lightweight entry metadata for streaming request planning (pass 1).
 * No content loaded — just enough to determine cut point. */
typedef struct {
    int64_t id;
    Role role;
    StopReason stop_reason;
    int token_estimate;     /* chars/4 heuristic from length(data) */
    int tool_call_count;    /* number of tool_calls in assistant msg (0 otherwise) */
} PlanEntry;

/* V41: Result of context_plan() — ordered entry IDs + cut point.
 * Entries are in root→leaf order after V28 filtering. */
typedef struct {
    PlanEntry *entries;     /* heap-allocated array */
    int count;              /* total entries after V28 filtering */
    int cut;               /* index of first included entry (skip entries[0..cut-1]) */
    int budget;            /* token budget used for planning */
} ContextPlan;

/* V41,V7,V8: Plan which entries to include in LLM request (pass 1).
 * Queries SQLite for branch metadata without loading content.
 * Applies V28 filtering and V8 cut logic.
 * Returns 0 on success, -1 on error. Caller frees with context_plan_free(). */
int context_plan(sqlite3 *db, int64_t session_id, const Config *cfg, ContextPlan *out);

/* Free a ContextPlan. */
void context_plan_free(ContextPlan *plan);

/* V7,V8: Select most recent messages from branch that fit within token budget.
 * - Budget defaults to 60% of config context_window.
 * - Never cuts mid-tool-call (V8): a tool_calls assistant msg and its
 *   corresponding tool result msgs are kept or dropped as a unit.
 * - If truncation occurs, a system message with cutoff notice is prepended.
 *
 * entries: full branch in root→leaf order (from session_get_branch)
 * count: number of entries
 * cfg: config (for context_window)
 * out_msgs: receives pointer to malloc'd Message array (caller frees with context_free)
 * out_count: receives number of messages in result
 *
 * Returns 0 on success, -1 on error. */
int context_build(const Entry *entries, int count, const Config *cfg,
                  Message **out_msgs, int *out_count);

/* Free message array returned by context_build. */
void context_free(Message *msgs, int count);

/* Estimate token count for a message (chars/4 heuristic). */
int context_estimate_tokens(const Message *msg);

/* V40: Truncate tool result content to 50KB / 2000 lines (whichever first).
 * Returns malloc'd truncated string with suffix, or strdup(src) if within limits.
 * Caller frees. */
char *truncate_result(const char *src, size_t len);

/* T118: Truncate tool result at write time. If content exceeds limits, writes
 * full output to /tmp/cclaw-<session_id>/<tool_call_id>.out and returns
 * truncated content with file path reference. Returns malloc'd string (caller frees).
 * If no truncation needed, returns strdup(src). */
char *truncate_and_spill(const char *src, int64_t session_id, const char *tool_call_id);

/* T118: Build session temp dir path: /tmp/cclaw-<session_id>
 * Writes into buf (must be >= 64 bytes). */
void session_tmp_dir(int64_t session_id, char *buf, size_t bufsz);

/* T118: Remove session temp dir recursively. */
void session_tmp_cleanup(int64_t session_id);

#endif
