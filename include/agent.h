#ifndef CCLAW_AGENT_H
#define CCLAW_AGENT_H

#include "types.h"
#include "arena.h"
#include "config.h"
#include "llm.h"
#include "db.h"

#define AGENT_DEFAULT_MAX_ITERATIONS 25

/* V10: Tool dispatch function. Returns heap-allocated result string.
 * On failure, must still return an error string (never NULL). */
typedef char *(*ToolDispatchFn)(const char *name, const char *arguments, void *user_data);

/* T115: Progress event types for mid-turn streaming */
typedef enum {
    PROGRESS_ASSISTANT_TEXT,  /* intermediate assistant text (thinking/content) */
    PROGRESS_TOOL_START,     /* tool call beginning (name + args) */
    PROGRESS_TOOL_RESULT     /* tool result (truncated for display) */
} ProgressEvent;

/* T115: Progress callback — called during agent loop for live display.
 * event: type of progress. name: tool name (TOOL_START/TOOL_RESULT) or NULL.
 * data: text content or truncated result. */
typedef void (*ProgressFn)(ProgressEvent event, const char *name,
                           const char *data, void *user_data);

/* Agent context for a single run */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const Config *cfg;
    ToolDispatchFn dispatch;
    void *dispatch_data;
    const ToolSchema *tools;
    size_t tool_count;
    int debug;              /* dump raw LLM req/resp JSON to stderr */
    ProgressFn progress;    /* T115: mid-turn progress callback (NULL = silent) */
    void *progress_data;
} AgentContext;

/* Run agent loop: call LLM, dispatch tool_calls, repeat until assistant
 * produces final content (no tool_calls) or max iterations reached.
 * The user message should already be appended to the session before calling.
 * Returns 0 on success, -1 on fatal error (LLM unreachable, OOM),
 * -2 on context overflow (caller should trim history and retry). */
int agent_run(AgentContext *ctx);

#endif
