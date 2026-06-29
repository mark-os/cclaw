#define _POSIX_C_SOURCE 200809L
#include "tools.h"
#include "tool_js.h"
#include "sqlite3.h"
#include <stdlib.h>
#include <string.h>

void tools_init(ToolRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
}

int tools_register(ToolRegistry *reg, const char *name, const char *description,
                   const char *parameters_json, ToolHandlerFn handler, void *user_data) {
    if (!reg || !name || reg->count >= TOOLS_MAX) return -1;

    ToolEntry *e = &reg->entries[reg->count];
    e->name = strdup(name);
    e->description = description ? strdup(description) : NULL;
    e->parameters_json = parameters_json ? strdup(parameters_json) : NULL;
    e->policy_json = NULL;
    e->handler = handler;
    e->user_data = user_data;
    reg->count++;
    return 0;
}

void tools_set_recipe(ToolRegistry *reg, const char *name, ToolRecipe recipe) {
    ToolEntry *e = tools_lookup(reg, name);
    if (e) e->recipe = recipe;
}

ToolEntry *tools_lookup(ToolRegistry *reg, const char *name) {
    if (!reg || !name) return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0)
            return &reg->entries[i];
    }
    return NULL;
}

size_t tools_schemas(ToolRegistry *reg, ToolSchema *out, size_t out_cap) {
    if (!reg || !out) return 0;
    size_t n = reg->count < out_cap ? reg->count : out_cap;
    for (size_t i = 0; i < n; i++) {
        out[i].name = reg->entries[i].name;
        out[i].description = reg->entries[i].description;
        out[i].parameters_json = reg->entries[i].parameters_json;
    }
    return n;
}

void tools_free(ToolRegistry *reg) {
    if (!reg) return;
    for (size_t i = 0; i < reg->count; i++) {
        free(reg->entries[i].name);
        free(reg->entries[i].description);
        free(reg->entries[i].parameters_json);
        free(reg->entries[i].policy_json);
        if (reg->entries[i].free_fn)
            reg->entries[i].free_fn(reg->entries[i].user_data);
    }
    reg->count = 0;
}

void tools_sync_to_db(ToolRegistry *reg, sqlite3 *db) {
    if (!reg || !db) return;
    /* Builtins only: the registry passed here holds C tools (extension tools are
     * loaded after this call). Write honest provenance — builtin=1, no
     * extension_name, no handler path. */
    static const char *sql =
        "INSERT INTO tools(name, extension_name, description, parameters_json, path, builtin) "
        "VALUES(?1, NULL, ?2, ?3, NULL, 1) "
        "ON CONFLICT(name) DO UPDATE SET "
        "parameters_json = excluded.parameters_json, "
        "description = COALESCE(tools.description, excluded.description), "
        "builtin = 1, extension_name = NULL";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;
    for (size_t i = 0; i < reg->count; i++) {
        ToolEntry *e = &reg->entries[i];
        if (!e->name || !e->parameters_json) continue;
        sqlite3_bind_text(stmt, 1, e->name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, e->description, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, e->parameters_json, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
}

void tools_load_extension_tools(ToolRegistry *reg, sqlite3 *db,
                                const char *agent_name, void *ectx) {
    if (!reg || !db || !agent_name) return;
    /* The registry is a cache of this query (specs/extensions.md#loading):
     * an agent sees a tool when it has the extension attached + enabled, and
     * the extension is published or owned by the agent. */
    static const char *sql =
        "SELECT t.name, t.description, t.parameters_json, t.path, t.policy "
        "FROM tools t "
        "JOIN agent_extensions ae ON ae.extension_name = t.extension_name "
        "JOIN extensions e ON e.name = t.extension_name "
        "WHERE ae.agent_name = ?1 AND ae.enabled = 1 AND t.enabled = 1 "
        "  AND t.path IS NOT NULL "
        "  AND (e.published = 1 OR e.owner_agent = ?1)";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(st, 1, agent_name, -1, SQLITE_STATIC);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(st, 0);
        const char *desc = (const char *)sqlite3_column_text(st, 1);
        const char *params = (const char *)sqlite3_column_text(st, 2);
        const char *path = (const char *)sqlite3_column_text(st, 3);
        const char *policy = (const char *)sqlite3_column_text(st, 4);
        if (!name || !path) continue;
        js_tool_register_ext(reg, name, desc, params ? params : "{}", path,
                             (JsEvalCtx *)ectx, policy);
    }
    sqlite3_finalize(st);
}

