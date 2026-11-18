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
 * provider's report is the measure: an entry's count is stored when a report
 * measures it (entries.tokens, db_response.c), the head is read off the
 * newest usable report (context.c), and this estimate stands only where no
 * report says. 2.5 bytes per token: across Claude, Gemini, DeepSeek and GPT
 * models the measured range is 1.05 (random base64 on Claude) to 6.9
 * (Cyrillic on Gemini); 2.5 reads ordinary text, code and JSON high on every
 * model but Claude, where it is close (plan/reference-reviews/token-estimate-
 * measurements-2026-11-18.md). Where it stands alone it errs high by policy:
 * an over-estimate compacts a little early, an under-estimate is an outage.
 *
 * This is the one definition. Kept as the integer ratio TOKEN_EST_NUM /
 * TOKEN_EST_DEN tokens per byte; C uses the macros below, SQL that needs the
 * number spells it with SQL_TOKEN_ESTIMATE, and the bundled compaction
 * script states the same 2.5. TOKEN_FRAMING is what one entry costs beyond
 * its text; the head has none (it is not an entry). Stored
 * entries.token_estimate is TOKEN_ESTIMATE(content_bytes), and
 * TOKEN_ESTIMATE(0) on a reasoning entry (its cost is its replay). */
#define TOKEN_EST_NUM 2
#define TOKEN_EST_DEN 5
#define TOKEN_FRAMING 4
#define TOKENS_OF_BYTES(len) ((int)((int64_t)(len) * TOKEN_EST_NUM / TOKEN_EST_DEN))
#define TOKEN_ESTIMATE(len) (TOKENS_OF_BYTES(len) + TOKEN_FRAMING)
/* The inverse, for a budget given in tokens and spent in bytes. */
#define BYTES_OF_TOKENS(t) ((int)((int64_t)(t) * TOKEN_EST_DEN / TOKEN_EST_NUM))
#define CCLAW_STR_(x) #x
#define CCLAW_STR(x) CCLAW_STR_(x)
/* The same rule over a SQL integer expression (a column, a length()). */
#define SQL_TOKEN_ESTIMATE(expr) \
    "((" expr ")*" CCLAW_STR(TOKEN_EST_NUM) "/" CCLAW_STR(TOKEN_EST_DEN) \
    "+" CCLAW_STR(TOKEN_FRAMING) ")"

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
    /* The routing entry's agent_models.reasoning_display: "summary" or
     * "none"; NULL reads as "summary" (the column's default). Asks for the
     * provider's reasoning summary where the wire format can (llm_payload.c)
     * and decides whether its text is kept on the reasoning entry. */
    const char *reasoning_display;
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
    int auto_recall;        /* FTS5 auto-recall (default 1) */
    int recall_max_tokens;  /* max tokens for recalled context (default 500) */
} Config;

#endif
