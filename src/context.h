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
    int tool_call_count;    /* number of tool_calls in assistant msg (0 otherwise) */
    int usage_in;           /* provider's prompt count, on a reported response only */
    unsigned char fresh;    /* written after the newest edit on the branch */
    unsigned char reset;    /* a summary, or an entry carrying an edit's record
                             * ($.turn_edits): a report older than it was sent
                             * another branch */
    int tokens;             /* its size: the provider's count once a report has
                             * measured it (entries.tokens), the estimate until
                             * then (token_estimate, chars/3 + 4) */
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

/* Trim notes carry data.source = TRIM_SOURCE (turn_edit.c, the trim). */
#define TRIM_SOURCE "fallback"

/* The governing routing entry's numbers (context_limits): its model's window
 * and when / how far the session is trimmed, defaults filled in. */
typedef struct {
    int window;     /* models.context_window, or the global default */
    int threshold;  /* agent_models.trim_threshold, or 50% of window */
    int target;     /* agent_models.prune_target, or 50% of threshold */
    int lifetime;   /* agent_models.cache_lifetime in seconds, 0 = unknown */
} ContextLimits;

/* Plan which entries to include in LLM request (pass 1).
 * Queries SQLite for branch metadata without loading content.
 * overhead: estimated head (tool definitions + system prompt); the provider's
 *           measure replaces it when larger (out->head).
 * The budget is cfg->max_history_tokens when set, else the session's agent's
 * trim_threshold (context_limits, cfg->context_window as the default window).
 * Returns 0 on success, -1 on error. Caller frees with context_plan_free(). */
int context_plan(sqlite3 *db, int64_t session_id, const Config *cfg, int overhead, ContextPlan *out);

/* The branch as the next request would send it, measured, with no cut:
 * entries (error/aborted responses and their results left out) at
 * COALESCE(tokens, token_estimate), and out->head — the provider's measure
 * of the head when usable, else `head`. The size of the session is
 * out->head plus the entries' tokens. Free with context_plan_free(). */
int context_measure(sqlite3 *db, int64_t session_id, int head, ContextPlan *out);

/* Free a ContextPlan. */
void context_plan_free(ContextPlan *plan);

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

/* The governing entry of the agent's routing list — its first enabled chat
 * model — read into *out, the 50% defaults filled in. default_window stands
 * for a model with no context_window, or an agent with no list. */
void context_limits(sqlite3 *db, const char *agent_name, int default_window,
                    ContextLimits *out);

char *context_auto_recall(sqlite3 *db, int64_t session_id, const char *user_msg,
                          int max_tokens);

#endif
