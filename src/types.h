#ifndef CCLAW_TYPES_H
#define CCLAW_TYPES_H

/* Foundational value types shared across modules: message roles, the
 * Message/ToolCall/ToolResult shapes, StopReason, and ShellSecret. No
 * behavior — just the vocabulary the rest of the code speaks.
 */

#include <stdint.h>
#include <stddef.h>
#include <time.h>

/* Token estimate for a byte length — the fallback, not the measure. The
 * provider reports usage on every response, and that is the measure: the
 * entries between two reported requests are sized by the difference of the
 * two counts (plan_measure, src/context.c), and this estimate is used only
 * where there is no report, or where the provider's figure is lower. The
 * divisor is 3, measured against real payloads on 2026-08-13 (100-107% of
 * Claude's count then, ~130% of DeepSeek's); on 2026-11-16 Claude counted
 * ~1.9x it on dense tool output. Where it stands alone it errs high by
 * policy: an over-estimate compacts a little early, an under-estimate is an
 * outage. Stored entries.token_estimate stays in these units. Every C and
 * SQL site must agree (plan/projects/context-accounting.md). */
#define CHARS_PER_TOKEN 3
#define TOKEN_ESTIMATE(len) ((int)((len) / CHARS_PER_TOKEN) + 4)

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
    ENDPOINT_GEMINI,        /* Native Gemini generateContent format */
    ENDPOINT_ANTHROPIC      /* Native Anthropic Messages API */
} EndpointType;

/* Provider/model config */
typedef struct {
    char *name;             /* providers.name — NULL when running on built-in defaults */
    char *base_url;
    char *api_key;
    /* Where api_key came from — "env:<VAR>" or "db:secrets", never any part of
     * the value. NULL when no key resolved. Diagnostics only. */
    char *api_key_source;
    char *model;
    int max_tokens;         /* max response tokens */
    CacheHints cache_hints; /* prompt cache control */
    EndpointType endpoint_type; /* wire format — see endpoint_type_name() */
    /* Reasoning-effort knob for the candidate being built, as routed:
     * model_id keys models.effort_map, reasoning_effort is the requested
     * level. Both NULL on the synthetic/default candidate — and a NULL
     * reasoning_effort means no effort field goes on the wire at all. */
    char *model_id;
    char *reasoning_effort;
    /* The routing entry's agent_models.cache_lifetime, seconds; 0 = unknown.
     * On the Anthropic wire 3600 sends the 1-hour cache marker, anything
     * else the plain 5-minute one. */
    int cache_lifetime;
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
    char *system_prompt;    /* per-agent system prompt, supports {session_id} {date} */
    int web_port;
    int context_window;     /* global default context window (overridden per-model in models table) */
    int max_iterations;     /* agent loop iteration cap */
    int max_history_tokens; /* request cut budget (0 = the governing trim_threshold) */
    int shell_timeout;      /* default shell_exec timeout in seconds (0 = 30) */
    int token_rate_limit;   /* max tokens/hr (default 1000000, 0=unlimited) */
    int64_t daily_cost_limit_nano; /* max spend/24h in nanodollars (0=unlimited) */
    LogLevel log_level;     /* error|warn|info|debug|trace */
    int save_reasoning;     /* store reasoning text as entries (default 0 = discard) */
    int auto_recall;        /* FTS5 auto-recall (default 1) */
    int recall_max_tokens;  /* max tokens for recalled context (default 500) */
} Config;

#endif
