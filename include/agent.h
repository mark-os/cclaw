#ifndef CCLAW_AGENT_H
#define CCLAW_AGENT_H

#include "types.h"
#include "arena.h"
#include "config.h"
#include "llm.h"
#include "db.h"

#define AGENT_MAX_ITERATIONS 25

/* V10: Tool dispatch function. Returns heap-allocated result string.
 * On failure, must still return an error string (never NULL). */
typedef char *(*ToolDispatchFn)(const char *name, const char *arguments, void *user_data);

/* Agent context for a single run */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const Config *cfg;
    ToolDispatchFn dispatch;
    void *dispatch_data;
    const ToolSchema *tools;
    size_t tool_count;
} AgentContext;

/* Run agent loop: call LLM, dispatch tool_calls, repeat until assistant
 * produces final content (no tool_calls) or max iterations reached.
 * The user message should already be appended to the session before calling.
 * Returns 0 on success, -1 on fatal error (LLM unreachable, OOM). */
int agent_run(AgentContext *ctx);

#endif
