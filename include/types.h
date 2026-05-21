#ifndef CCLAW_TYPES_H
#define CCLAW_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Message roles */
typedef enum {
    ROLE_SYSTEM,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL
} Role;

/* Tool call within an assistant message */
typedef struct {
    char *id;
    char *name;
    char *arguments;    /* raw JSON string */
} ToolCall;

/* Tool result content */
typedef struct {
    char *tool_call_id;
    char *content;
} ToolResult;

/* Single message in conversation */
typedef struct {
    Role role;
    char *content;          /* text content (NULL if tool_calls only) */
    ToolCall *tool_calls;   /* array (assistant msgs only, NULL otherwise) */
    size_t tool_call_count;
    ToolResult *tool_result; /* tool role msgs only, NULL otherwise */
} Message;

/* V14: session tree entry — id + parent_id for branching structure */
typedef struct {
    int64_t id;
    int64_t parent_id;      /* -1 = root (no parent) */
    int64_t session_id;
    time_t created_at;
    Message message;
} Entry;

/* Session metadata */
typedef struct {
    int64_t id;
    char *name;
    int64_t leaf_id;        /* current leaf entry id for branch traversal */
    time_t created_at;
    time_t updated_at;
} Session;

/* Provider/model config */
typedef struct {
    char *base_url;
    char *api_key;
    char *model;
    int max_tokens;         /* max response tokens */
    int context_window;     /* model context window size */
} ProviderConfig;

/* Top-level config */
typedef struct {
    ProviderConfig provider;
    ProviderConfig *fallback_providers; /* T45: fallback chain (heap array) */
    size_t fallback_count;
    char *db_path;
    char *workspace;
    char *telegram_token;
    char *system_prompt;    /* T46: per-agent system prompt, supports {session_id} {date} */
    int web_port;
    int max_iterations;     /* agent loop iteration cap */
    int max_history_tokens; /* V7: token budget for context (0 = 60% of context_window) */
    int heartbeat_interval; /* seconds between heartbeat system msgs (0=disabled) */
    int shell_timeout;      /* default shell_exec timeout in seconds (0 = 30) */
    int debug;              /* --debug: dump raw LLM req/resp JSON to stderr */
} Config;

#endif
