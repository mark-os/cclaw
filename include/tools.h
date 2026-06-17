#ifndef CCLAW_TOOLS_H
#define CCLAW_TOOLS_H

#include "llm.h"
#include "sqlite3.h"
#include <stddef.h>

#define TOOLS_MAX 32

/* Tool handler function. Returns heap-allocated result string (never NULL). */
typedef char *(*ToolHandlerFn)(const char *arguments, void *user_data);

/* Optional destructor for user_data */
typedef void (*ToolFreeFn)(void *user_data);

/* Single tool entry in the registry */
typedef struct {
    char *name;
    char *description;
    char *parameters_json;
    ToolHandlerFn handler;
    void *user_data;
    ToolFreeFn free_fn;
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

/* Get schema array for LLM request. Writes schemas into caller-provided array.
 * Returns number of schemas written (up to out_cap). */
size_t tools_schemas(ToolRegistry *reg, ToolSchema *out, size_t out_cap);


/* Sync built-in tool schemas to the DB `tools` table.
 * parameters_json is code-owned: force-overwrite it from the registry.
 * description is DB-editable: fill only if the row is missing/NULL, never clobber. */
void tools_sync_to_db(ToolRegistry *reg, sqlite3 *db);

/* Free all heap strings in the registry */
void tools_free(ToolRegistry *reg);

#endif
