#ifndef CCLAW_TYPES_H
#define CCLAW_TYPES_H

/* Foundational value types shared across modules: message roles, the
 * Message/ToolCall/ToolResult shapes, StopReason, and ShellSecret. No
 * behavior — just the vocabulary the rest of the code speaks.
 */

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Message roles */
typedef enum {
    ROLE_SYSTEM,
    ROLE_USER,
    ROLE_ASSISTANT,
    ROLE_TOOL,
    ROLE_COMPACTION   /* summary node — CTE stops here */
} Role;

/* normalized stop reason (provider-agnostic) */
typedef enum {
    STOP_REASON_NONE = 0,   /* not set / not applicable */
    STOP_REASON_STOP,       /* normal completion */
    STOP_REASON_LENGTH,     /* hit max tokens */
    STOP_REASON_TOOL_USE,   /* wants tool calls */
    STOP_REASON_ERROR,      /* provider error, content_filter, parse failure */
    STOP_REASON_ABORTED     /* client-side abort (SIGTERM, timeout) */
} StopReason;

/* log levels (increasing verbosity) */
typedef enum {
    LOG_LEVEL_ERROR = 0,    /* errors only */
    LOG_LEVEL_WARN,         /* + degraded-but-continuing conditions */
    LOG_LEVEL_INFO,         /* + turn boundaries */
    LOG_LEVEL_DEBUG,        /* + timing, retry decisions, response shapes, context stats */
    LOG_LEVEL_TRACE         /* + full req/resp JSON */
} LogLevel;

/* Secret name/value pair for injection into tool children + output masking */
typedef struct {
    char *name;   /* e.g. "GITHUB_TOKEN" (without CCLAW_SECRET_ prefix) */
    char *value;  /* plaintext secret value */
} ShellSecret;

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
    StopReason stop_reason; /* assistant msgs only, STOP_REASON_NONE otherwise */
    char *metadata_json;    /* optional: reasoning, usage, logprobs (raw JSON object) */
    /* split-column fields written directly at insert time */
    const char *model;      /* assistant msgs: which model produced this */
    const char *tool_name;  /* tool msgs: which tool produced this result */
    int is_error;           /* tool msgs: whether result is an error */
    int usage_in;           /* assistant msgs: input tokens */
    int usage_out;          /* assistant msgs: output tokens */
    int64_t cost_nano;      /* assistant msgs: cost in nanodollars */
} Message;

/* session tree entry — id + parent_id for branching structure */
typedef struct {
    int64_t id;
    int64_t parent_id;      /* -1 = root (no parent) */
    int64_t original_parent_id; /* set on reparent, -1 = never reparented */
    int64_t session_id;
    time_t created_at;
    Message message;
} Entry;

/* Session metadata */
typedef struct {
    int64_t id;
    char *name;
    char *agent_name;       /* bound agent (NULL = global config) */
    int64_t leaf_id;        /* current leaf entry id for branch traversal */
    time_t created_at;
    time_t updated_at;
} Session;

/* cache hint strategy */
typedef enum {
    CACHE_HINTS_AUTO = 0,   /* provider-native implicit caching (default) */
    CACHE_HINTS_ON,         /* force cache markers (e.g. Anthropic cache_control) */
    CACHE_HINTS_OFF,        /* disable all cache markers */
} CacheHints;

/* provider endpoint type (wire format) */
typedef enum {
    ENDPOINT_OPENAI = 0,    /* OpenAI-compatible chat completions (default) */
    ENDPOINT_GEMINI         /* Native Gemini generateContent format */
} EndpointType;

/* Provider/model config */
typedef struct {
    char *base_url;
    char *api_key;
    char *model;
    int max_tokens;         /* max response tokens */
    CacheHints cache_hints; /* prompt cache control */
    EndpointType endpoint_type; /* wire format (openai or gemini) */
} ProviderConfig;

/* Top-level config */
typedef struct {
    ProviderConfig provider;
    ProviderConfig *fallback_providers; /* fallback chain (heap array) */
    size_t fallback_count;
    char *db_path;
    char *workspace;
    /* Operator pinned the workspace (config key or CCLAW_WORKSPACE) rather than
     * it coming from default_workspace(). Suppresses per-agent derivation so a
     * CLI/test-pinned workspace keeps serving every agent. */
    int workspace_explicit;
    char *telegram_token;
    char *system_prompt;    /* per-agent system prompt, supports {session_id} {date} */
    int web_port;
    int context_window;     /* global default context window (overridden per-model in models table) */
    int max_iterations;     /* agent loop iteration cap */
    int max_history_tokens; /* token budget for context (0 = 60% of context_window) */
    int shell_timeout;      /* default shell_exec timeout in seconds (0 = 30) */
    int stale_lock_timeout; /* janitor stale lock threshold in seconds (0 = 300) */
    float context_threshold;  /* trigger compaction/truncation when tokens > this × context_window (default 0.6) */
    float compaction_target;  /* compact down to this × context_window (default 0.3) */
    int compaction;           /* 1=summarize via LLM (default), 0=truncate only */
    int token_rate_limit;   /* max tokens/hr (default 1000000, 0=unlimited) */
    int64_t daily_cost_limit_nano; /* max spend/24h in nanodollars (0=unlimited) */
    LogLevel log_level;     /* error|warn|info|debug|trace */
    int save_reasoning;     /* store reasoning text as entries (default 0 = discard) */
    int auto_recall;        /* FTS5 auto-recall (default 1) */
    int recall_max_tokens;  /* max tokens for recalled context (default 500) */
} Config;

#endif
