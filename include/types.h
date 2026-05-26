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
    ROLE_TOOL,
    ROLE_COMPACTION   /* V58: summary node — CTE stops here */
} Role;

/* V35: normalized stop reason (provider-agnostic) */
typedef enum {
    STOP_REASON_NONE = 0,   /* not set / not applicable */
    STOP_REASON_STOP,       /* normal completion */
    STOP_REASON_LENGTH,     /* hit max tokens */
    STOP_REASON_TOOL_USE,   /* wants tool calls */
    STOP_REASON_ERROR,      /* provider error, content_filter, parse failure */
    STOP_REASON_ABORTED     /* client-side abort (SIGTERM, timeout) */
} StopReason;

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
    StopReason stop_reason; /* V35: assistant msgs only, STOP_REASON_NONE otherwise */
    char *metadata_json;    /* optional: reasoning, usage, logprobs (raw JSON object) */
} Message;

/* V14: session tree entry — id + parent_id for branching structure */
typedef struct {
    int64_t id;
    int64_t parent_id;      /* -1 = root (no parent) */
    int64_t original_parent_id; /* V59: set on reparent, -1 = never reparented */
    int64_t session_id;
    time_t created_at;
    Message message;
} Entry;

/* Session metadata */
typedef struct {
    int64_t id;
    char *name;
    char *agent_name;       /* V20: bound agent (NULL = global config) */
    int64_t leaf_id;        /* current leaf entry id for branch traversal */
    time_t created_at;
    time_t updated_at;
} Session;

/* T123: cache hint strategy */
typedef enum {
    CACHE_HINTS_AUTO = 0,   /* detect from model name */
    CACHE_HINTS_ON,         /* force cache markers */
    CACHE_HINTS_OFF         /* disable cache markers */
} CacheHints;

/* Provider/model config */
typedef struct {
    char *base_url;
    char *api_key;
    char *model;
    int max_tokens;         /* max response tokens */
    int context_window;     /* model context window size */
    CacheHints cache_hints; /* T123: prompt cache control */
} ProviderConfig;

/* Top-level config */
typedef struct {
    ProviderConfig provider;
    ProviderConfig *fallback_providers; /* T45: fallback chain (heap array) */
    size_t fallback_count;
    char *db_path;
    char *workspace;
    char *telegram_token;
    char *telegram_base_url; /* override for testing (default: https://api.telegram.org) */
    int64_t *admin_chat_ids; /* V53: Telegram admin chat IDs (heap array) */
    size_t admin_chat_id_count;
    char *system_prompt;    /* T46: per-agent system prompt, supports {session_id} {date} */
    char *env_file;         /* T141: path to env file for key writes (default /etc/cclaw/env) */
    int web_port;
    int max_iterations;     /* agent loop iteration cap */
    int max_history_tokens; /* V7: token budget for context (0 = 60% of context_window) */
    int heartbeat_interval; /* seconds between heartbeat system msgs (0=disabled) */
    int shell_timeout;      /* default shell_exec timeout in seconds (0 = 30) */
    int stale_lock_timeout; /* janitor stale lock threshold in seconds (0 = 300) */
    int debug;              /* --debug: dump raw LLM req/resp JSON to stderr */
    int save_reasoning;     /* store reasoning/thinking tokens in entry metadata */
    int save_usage;         /* store token usage in entry metadata */
    int save_logprobs;      /* store logprobs in entry metadata */
} Config;

#endif
