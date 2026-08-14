#define _POSIX_C_SOURCE 200809L
#include "tools.h"
#include "tool_js.h"
#include "extension_manifest.h"
#include "log.h"
#include "sqlite3.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tools_init(ToolRegistry *reg) {
    memset(reg, 0, sizeof(*reg));
}

char *tool_errf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}

char *tool_fail(int *is_error, const char *fmt, ...) {
    if (is_error) *is_error = 1;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
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

char *tool_sandboxed_stub(const char *arguments, void *user_data, int *is_error) {
    (void)arguments; (void)user_data;
    return tool_fail(is_error, "error: sandboxed tool dispatched in-process (wiring bug)");
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
     * loaded after this call). Write honest provenance — no extension_name, no
     * handler path (that's what distinguishes a C tool from a JS one). */
    static const char *sql =
        "INSERT INTO tools(name, extension_name, description, parameters_json, path) "
        "VALUES(?1, NULL, ?2, ?3, NULL) "
        "ON CONFLICT(name) DO UPDATE SET "
        "parameters_json = excluded.parameters_json, "
        /* Code owns builtin schema AND description — the model renders from
         * this table (llm_payload), so a stale seeded description would keep
         * advertising options the code no longer accepts. */
        "description = COALESCE(excluded.description, tools.description), "
        "extension_name = NULL";
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
        /* Copy-on-promote (Q4): only the shared store holds promoted code. A
         * row pointing anywhere else — a hand-edited DB, a workspace draft
         * path — is skipped, not loaded: the approval covered the copy in the
         * store, and nothing else. */
        if (!extension_path_in_store(db, path)) {
            LOG_WARN_("tool '%s': handler path outside the shared extension "
                      "store, not loaded (%s)", name, path);
            continue;
        }
        /* Idempotent: dispatch re-runs this query on a lookup miss (extension
         * promoted mid-process), so skip names already materialized. */
        if (tools_lookup(reg, name)) continue;
        js_tool_register_ext(reg, name, desc, params ? params : "{}", path,
                             (JsEvalCtx *)ectx, policy);
    }
    sqlite3_finalize(st);
}

/* Mirrors channel_config_get(): the registry holds an extension's settings as
 * <extension>.<key> rows, and the JS side sees them with the prefix stripped.
 * secret=1 rows are excluded — a secret reaches a tool child by {{SECRET:name}}
 * interpolation at dispatch, not by riding along with every call. The prefix
 * match is substr(), not LIKE: an extension named foo_bar must not match
 * fooXbar's keys. */
char *tool_extension_config_json(sqlite3 *db, const char *tool_name) {
    if (!db || !tool_name) return NULL;
    static const char *sql =
        "SELECT json_group_object(substr(c.key, length(t.extension_name) + 2),"
        "                         COALESCE(c.value, c.default_value))"
        " FROM tools t JOIN config c"
        "   ON substr(c.key, 1, length(t.extension_name) + 1) ="
        "      t.extension_name || '.'"
        " WHERE t.name = ?1 AND COALESCE(t.extension_name, '') <> ''"
        "   AND c.secret = 0;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(st, 1, tool_name, -1, SQLITE_STATIC);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out ? out : strdup("{}");
}

