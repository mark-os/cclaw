#ifndef CCLAW_TOOLS_H
#define CCLAW_TOOLS_H

#include "llm.h"
#include "sqlite3.h"
#include <stddef.h>

#define TOOLS_MAX 48

/* Tool handler function. Returns heap-allocated result string (never NULL). */
typedef char *(*ToolHandlerFn)(const char *arguments, void *user_data);

/* Optional destructor for user_data */
typedef void (*ToolFreeFn)(void *user_data);

/* ── Execution recipe (axis A: vehicle, axis B: sandbox tier) ──────────
 * Each tool carries its own recipe — the dispatcher reads it instead of
 * comparing names. See specs/tool-execution-model.md and the sandbox plan.
 *
 * EXEC_INLINE   — run on the poll thread (control-flow holdouts that read live
 *                 session state or mutate the registry: launch_agent,
 *                 check_session, check_approval, request_config, extension_*).
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
                        int64_t session_id, const char *args);
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

/* Get schema array for LLM request. Writes schemas into caller-provided array.
 * Returns number of schemas written (up to out_cap). */
size_t tools_schemas(ToolRegistry *reg, ToolSchema *out, size_t out_cap);


/* Sync built-in tool schemas to the DB `tools` table (builtin=1,
 * extension_name=NULL). parameters_json is code-owned: force-overwrite it from
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

/* Free all heap strings in the registry */
void tools_free(ToolRegistry *reg);

#endif
