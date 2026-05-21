#ifndef CCLAW_TOOLS_H
#define CCLAW_TOOLS_H

#include "llm.h"
#include <stddef.h>

#define TOOLS_MAX 32

/* Tool handler function. Returns heap-allocated result string (never NULL). */
typedef char *(*ToolHandlerFn)(const char *arguments, void *user_data);

/* Single tool entry in the registry */
typedef struct {
    char *name;
    char *description;
    char *parameters_json;
    ToolHandlerFn handler;
    void *user_data;
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

/* Get schema array suitable for llm_build_request. Writes count to *out_count.
 * Returns pointer to internal static array (valid until next call). */
const ToolSchema *tools_schemas(ToolRegistry *reg, size_t *out_count);

/* Free all heap strings in the registry */
void tools_free(ToolRegistry *reg);

#endif
