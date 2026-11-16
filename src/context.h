#ifndef CCLAW_CONTEXT_H
#define CCLAW_CONTEXT_H

/* Request planning: decides which entries fit the model's context window
 * and where to cut, using lightweight per-entry metadata (no content
 * loaded). Feeds the payload builder.
 */

#include "types.h"
#include "sqlite3.h"
#include <stddef.h>

/* Lightweight entry metadata for streaming request planning (pass 1).
 * No content loaded — just enough to determine cut point and size. */
typedef struct {
    int64_t id;
    int64_t turn_id;        /* the turn this entry belongs to — cut boundaries key on it */
    int64_t iteration_id;
    Role role;
    StopReason stop_reason;
    int token_estimate;     /* TOKEN_ESTIMATE of the stored text (chars/3 + 4) */
    int tool_call_count;    /* number of tool_calls in assistant msg (0 otherwise) */
    int usage_in;           /* provider's prompt count, on a reported response only */
    int usage_out;          /* less the reasoning tokens reported inside it */
    int epoch;              /* reports: compactions on the branch older than it */
    int model;              /* reports: model run — bumps when a report's model
                             * differs from the previous report's */
    unsigned char fresh;    /* written after the newest compaction on the branch */
    unsigned char turn_edited; /* its turn carries a turn-edit record */
    unsigned char repointed;   /* original_parent_id set: compaction or an edit
                                * changed what precedes it */
    int tokens;             /* measured size: the estimate, plus — on a reported
                             * response — what the provider counted above the
                             * estimate for its segment (context.c, plan_measure) */
} PlanEntry;

/* Result of context_plan() — ordered entry IDs + cut point. */
typedef struct {
    PlanEntry *entries;     /* heap-allocated array */
    int count;              /* total entries after filtering */
    int cut;               /* index of first included entry (skip entries[0..cut-1]) */
    int budget;            /* token budget used for planning */
    int head;              /* head the cut reserved: the estimate passed in, or
                            * the provider's measure of it when that is larger */
} ContextPlan;

/* Plan which entries to include in LLM request (pass 1).
 * Queries SQLite for branch metadata without loading content.
 * overhead: estimated head (tool definitions + system prompt); the provider's
 *           measure replaces it when larger (out->head).
 * context_window: effective context window for this request (0 = use cfg default).
 * Returns 0 on success, -1 on error. Caller frees with context_plan_free(). */
int context_plan(sqlite3 *db, int64_t session_id, const Config *cfg, int overhead, ContextPlan *out);

/* Free a ContextPlan. */
void context_plan_free(ContextPlan *plan);

/* Index of the first entry compaction keeps: walk the tail back until
 * target_tokens is spent, then snap to a turn boundary. Returns 0 when there is
 * nothing to compact (the whole branch fits, or it is a single turn). Lives here
 * next to the request cut so both boundary rules read the same way. */
int context_compaction_keep_from(const ContextPlan *plan, int target_tokens);

/* Truncate tool result at write time. If content exceeds limits, writes
 * full output to <session tmp dir>/<tool_call_id>.out and returns truncated
 * content with file path reference. Returns malloc'd string (caller frees). */
char *truncate_and_spill(sqlite3 *db, const char *src, int64_t session_id,
                         const char *tool_call_id);

/* Resolve the spill file path for one tool call and create its directory.
 * The sandboxed child receives the result of this over the wire rather than
 * building it itself — the model-emitted call_id is sanitised here, in the
 * trusted parent. Returns 0 on success. */
/* Background-job live log path: the spill file's sibling with a .log suffix.
 * Returns 0 on success (directory created). */
int job_log_path_build(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                       char *buf, size_t bufsz);

int spill_path_build(sqlite3 *db, int64_t session_id, const char *tool_call_id,
                     char *buf, size_t bufsz);

/* Last `cap` bytes of a background job's log — the completion notice's and
 * check_session's output tail. Heap; caller frees. NULL if unreadable/empty. */
char *job_log_tail(sqlite3 *db, int64_t session_id, const char *call_id,
                   size_t cap);

/* Build the spill dir for a session: the advancing agent's workspace
 * .tool_results/<session_id> (sandbox-visible), CCLAW_WORKSPACE when the
 * operator pinned one, /tmp/cclaw-<session_id> only as db-less fallback. */
void session_tmp_dir(sqlite3 *db, int64_t session_id, char *buf, size_t bufsz);

/* Auto-recall — FTS5 search across sessions for relevant context.
 * Returns heap-allocated text (caller frees) or NULL if nothing recalled. */
/* Estimated tokens of the request head (tools array + system prompt) for
 * this agent — the part of a request that is never an entry. Pass the
 * system prompt when already built, NULL to build it here. */
int context_head_estimate(sqlite3 *db, const char *agent_name, int64_t session_id,
                          const Config *cfg, const char *system_prompt);

/* Compaction trigger: the context's measured size (head + entries, the
 * provider's count where it reported, the estimate elsewhere), or the sent
 * payload /3, exceeds context_threshold x window less the last turn. */
int session_needs_compaction(sqlite3 *db, int64_t session_id, const Config *cfg);

/* 1 when the leaf turn's measured size no longer fits in window - head, so
 * no cut can rescue the next request. *turn_tokens receives the turn's size. */
int context_turn_overflows(sqlite3 *db, int64_t session_id, int window, int head,
                           int *turn_tokens);

/* Resolve the effective context window for an agent's model.
 * Priority: models.context_window (non-NULL) → global_default.
 * Returns global_default if agent has no model or model has NULL window. */
int model_context_window(sqlite3 *db, const char *agent_name, int global_default);

char *context_auto_recall(sqlite3 *db, int64_t session_id, const char *user_msg,
                          int max_tokens);

#endif
