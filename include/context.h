#ifndef CCLAW_CONTEXT_H
#define CCLAW_CONTEXT_H

#include "types.h"
#include <stddef.h>

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

#endif
