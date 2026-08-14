#ifndef CCLAW_TOOLS_H
#define CCLAW_TOOLS_H

/* The tool registry — the dispatch table mapping tool name to handler,
 * parameters schema, policy, and execution recipe (vehicle + sandbox tier).
 * The spine every tool module registers into.
 */

#include "llm.h"
#include "sqlite3.h"
#include <stddef.h>

#define TOOLS_MAX 48

/* Ceiling on a caller-supplied `timeout` argument, shared by every tool that
 * takes one (shell_exec, web_fetch, js_eval). A tool call is one step of a
 * turn, not a background job: past ten minutes the right answer is a
 * detached script, not a longer wait. Requests above the cap are clamped, not
 * refused — the model asked for "long", and it gets the longest we do. */
#define TOOL_TIMEOUT_MAX_SEC 600

/* Tool handler function. Returns heap-allocated result string (never NULL).
 *
 * Failure is signalled EXPLICITLY through *is_error, set at the failure site
 * (tool_fail below does both in one expression). The dispatcher presets it to
 * 0 and writes it straight through to entries.is_error. Result TEXT is
 * free-form: the "error: ..." wording survives only for readability — nothing
 * anywhere parses it (the old strncmp("error:") sniff is gone, and a failure
 * message that doesn't happen to start with that word is still a failure).
 * `is_error` may be NULL for callers outside the dispatcher (tests). */
typedef char *(*ToolHandlerFn)(const char *arguments, void *user_data, int *is_error);

/* Optional destructor for user_data */
typedef void (*ToolFreeFn)(void *user_data);

/* ── Execution recipe (axis A: vehicle, axis B: sandbox tier) ──────────
 * Each tool carries its own recipe — the dispatcher reads it instead of
 * comparing names. See specs/tool-execution-model.md and the sandbox plan.
 *
 * EXEC_INLINE   — run on the poll thread (control-flow holdouts that read live
 *                 session state or mutate the registry: launch_agent,
 *                 check_session, request_config, extension_*).
 * EXEC_THREAD   — fire-and-forget detached thread; DB/session-only tools.
 * EXEC_SANDBOX  — fork+execve --run-tool child (file/shell/web/js). */
typedef enum { EXEC_INLINE, EXEC_THREAD, EXEC_SANDBOX } ExecVehicle;
typedef enum { SBX_NONE, SBX_FILE, SBX_WEB, SBX_SHELL, SBX_JS } SandboxTier;

typedef struct {
    ExecVehicle  vehicle;
    SandboxTier  tier;                 /* meaningful iff EXEC_SANDBOX */
    /* iff EXEC_THREAD: rebuilds the tool's ctx around the thread's own db +
     * agent_name (+ session_id for the few tools that need it) and runs the
     * handler. */
    char *(*thread_run)(sqlite3 *db, const char *agent_name,
                        int64_t session_id, const char *args,
                        int *is_error);
} ToolRecipe;

/* Single tool entry in the registry */
typedef struct {
    char *name;
    char *description;
    char *parameters_json;
    char *policy_json;
    ToolHandlerFn handler;
    void *user_data;
    ToolFreeFn free_fn;
    ToolRecipe recipe;
} ToolEntry;

/* Tool registry */
typedef struct {
    ToolEntry entries[TOOLS_MAX];
    size_t count;
} ToolRegistry;

/* Initialize an empty registry */
void tools_init(ToolRegistry *reg);

/* Register a tool. Returns 0 on success, -1 if full or name is NULL. */
int tools_register(ToolRegistry *reg, const char *name, const char *description,
                   const char *parameters_json, ToolHandlerFn handler, void *user_data);

/* Lookup a tool by name. Returns pointer to entry or NULL if not found. */
ToolEntry *tools_lookup(ToolRegistry *reg, const char *name);

/* Set a tool's execution recipe (no-op if the name isn't registered). The
 * recipe is the single source of truth for how the dispatcher runs a tool. */
void tools_set_recipe(ToolRegistry *reg, const char *name, ToolRecipe recipe);

/* Sync built-in tool schemas to the DB `tools` table (extension_name=NULL).
 * parameters_json is code-owned: force-overwrite it from
 * the registry. description is DB-editable: fill only if the row is
 * missing/NULL, never clobber. Call with a registry holding only builtins
 * (before loading extension tools), so extension rows are never clobbered. */
void tools_sync_to_db(ToolRegistry *reg, sqlite3 *db);

/* Materialize this agent's extension tools from the DB registry into `reg`.
 * Runs the tools⨝agent_extensions⨝extensions join (enabled + published-or-owned)
 * and registers each as a path-based JS tool (handler in the shared store),
 * carrying its policy. `ectx` is a JsEvalCtx* (opaque here to avoid an include
 * cycle). No JS is evaluated — loading is reading rows, not running code. */
void tools_load_extension_tools(ToolRegistry *reg, sqlite3 *db,
                                const char *agent_name, void *ectx);

/* The owning extension's config as {"key":"value",...} — what the tool JS
 * surface's getConfig() reads. Resolved parent-side because the sandboxed
 * --run-tool child never opens the DB; the object crosses as a wire param like
 * every other. Returns "{}" (never NULL, except on OOM/bad args) for a built-in
 * tool or an extension with no config rows. Caller frees. */
char *tool_extension_config_json(sqlite3 *db, const char *tool_name);

/* Free all heap strings in the registry */
void tools_free(ToolRegistry *reg);

/* Registered as the handler for sandboxed-tier tools: the dispatcher never
 * runs those in-process (EXEC_SANDBOX goes through the --run-tool broker,
 * which consumes pre-extracted wire params), so any call here is a wiring
 * bug — fail loudly rather than parse arguments. */
char *tool_sandboxed_stub(const char *arguments, void *user_data, int *is_error);

/* Formatted message for tool-handler returns (heap; caller frees). Truncates
 * at 512 bytes — handler messages are short human-readable lines. */
char *tool_errf(const char *fmt, ...);

/* The failure return: marks the call failed (*is_error = 1, NULL-tolerant) and
 * returns the formatted message. Every handler failure site goes through this
 * or sets *is_error itself — status and text leave together, so no reader ever
 * has to guess from the prose. */
char *tool_fail(int *is_error, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#endif
