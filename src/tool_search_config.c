/* search_config tool — read-only introspection of agent config and available tools. */
#define _POSIX_C_SOURCE 200809L
#include "tool_search_config.h"
#include "tool_parse.h"
#include "buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"Optional substring to filter tools by name/description\"}"
    "}}";

static char *handler(const char *arguments, void *user_data) {
    SearchConfigCtx *ctx = (SearchConfigCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: search_config unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON");
    const char *query = targ_str(&ta, "query");
    /* Treat empty string as no filter */
    if (query && !query[0]) query = NULL;

    Buf out = {0};

    /* Section 1: current grants */
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(ctx->db,
        "SELECT trust_level FROM agents WHERE name=?1", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *trust = (const char *)sqlite3_column_text(st, 0);
            buf_appendf(&out,
                "## Your current grants (agent: %s)\n"
                "trust_level: %s\n",
                ctx->agent_name,
                trust ? trust : "(unknown)");
        }
        sqlite3_finalize(st);
    }

    /* Report grants per kind */
    static const char *kinds[] = {"host", "tool", "read_path", "write_path"};
    for (int ki = 0; ki < 4; ki++) {
        rc = sqlite3_prepare_v2(ctx->db,
            "SELECT COALESCE(group_concat(value, ', '), '(none)')"
            " FROM grants WHERE agent_name=?1 AND kind=?2",
            -1, &st, NULL);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, kinds[ki], -1, SQLITE_STATIC);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(st, 0);
                buf_appendf(&out, "%ss: %s\n", kinds[ki], v ? v : "(none)");
            }
            sqlite3_finalize(st);
        }
    }

    /* Section 2: tools list */
    buf_appendf(&out, "\n## Tools you can use or request\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT name, description FROM tools"
        " WHERE (?1 IS NULL OR name LIKE '%'||?1||'%' OR description LIKE '%'||?1||'%')"
        " ORDER BY name", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        if (query)
            sqlite3_bind_text(st, 1, query, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(st, 1);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            const char *desc = (const char *)sqlite3_column_text(st, 1);
            buf_appendf(&out, "%s — %s\n", name, desc ? desc : "");
        }
        sqlite3_finalize(st);
    }

    /* Section 3: usage hint */
    buf_appendf(&out,
        "\n## Requesting changes (use the request_config tool)\n"
        "- grant a tool:  {\"action\":\"grant_tool\",\"tool\":\"<name>\"}\n"
        "- allow a host:  {\"action\":\"grant_host\",\"host\":\"<hostname>\"}\n"
        "- grant a path:  {\"action\":\"grant_path\",\"path\":\"/absolute/path\"}\n"
        "- rename agent:  {\"action\":\"rename_agent\",\"name\":\"<new_name>\"}\n"
        "All gated actions require human approval before taking effect.\n");

    tool_parse_free(&ta);
    char *result = buf_take(&out);
    return result ? result : strdup("error: out of memory");
}

/* EXEC_THREAD shim: rebuild SearchConfigCtx around the thread's own db. */
static char *search_config_thread_run(sqlite3 *db, const char *agent_name,
                                      int64_t session_id, const char *args) {
    (void)session_id;
    SearchConfigCtx c = {.db = db, .agent_name = agent_name};
    return handler(args, &c);
}

int tool_search_config_register(ToolRegistry *reg, SearchConfigCtx *ctx) {
    int rc = tools_register(reg, "search_config",
        "Discover your current configuration and what you can request: your trust level, "
        "granted tools and hosts, the full list of available tools, and how to request more "
        "via request_config. Optional 'query' filters the tool list.",
        PARAMS_JSON, handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "search_config",
                         (ToolRecipe){EXEC_THREAD, SBX_NONE, search_config_thread_run});
    return rc;
}
