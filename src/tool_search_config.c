/* search_config tool — read-only introspection of agent config and available tools. */
#define _POSIX_C_SOURCE 200809L
#include "tool_search_config.h"
#include "tool_args.h"
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

    char *query = tool_args_str(ctx->db, arguments, "query");
    /* Treat empty string as no filter */
    if (query && !query[0]) { free(query); query = NULL; }

    Buf out = {0};

    /* Section 1: current grants */
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(ctx->db,
        "SELECT sandbox_profile FROM agents WHERE name=?1", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *trust = (const char *)sqlite3_column_text(st, 0);
            /* agent_setup.c resolves CCLAW_SHELL_PATH (env override, else the
             * agents.shell_path column) and setenv's it either way — reading
             * it back here is the actual effective value, not a re-derivation. */
            const char *shell_path = getenv("CCLAW_SHELL_PATH");
            buf_appendf(&out,
                "## Your current grants (agent: %s)\n"
                "sandbox_profile: %s\n"
                "shell_path: %s\n",
                ctx->agent_name,
                trust ? trust : "(unknown)",
                (shell_path && shell_path[0]) ? shell_path : "/bin/sh");
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

    /* Sensitivity labels (global, operator-owned): shown so the model knows
     * why calls touching these targets park regardless of grants. */
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT COALESCE(group_concat(value, ', '), '(none)')"
        " FROM sensitive_targets WHERE kind='host'", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            buf_appendf(&out,
                "sensitive_hosts: %s (every call touching these parks for"
                " approval; grants never bypass this)\n", v ? v : "(none)");
        }
        sqlite3_finalize(st);
    }

    /* Secret-host bindings: which hosts each secret may be submitted to.
     * A {{SECRET:X}} aimed anywhere else parks for approval. */
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT COALESCE(group_concat(secret_name || '->' || host, ', '), '(none)')"
        " FROM secret_hosts", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            buf_appendf(&out,
                "secret_bindings: %s (a secret submitted to an unbound host"
                " parks for approval)\n", v ? v : "(none)");
        }
        sqlite3_finalize(st);
    }

    /* Section 2: tools list with grant status */
    buf_appendf(&out, "\n## Tools you can use or request\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT t.name, t.description,"
        "       (g.agent_name IS NOT NULL) AS granted,"
        "       g.approval_mode"
        " FROM tools t"
        " LEFT JOIN grants g ON g.agent_name=?2 AND g.kind='tool' AND g.value=t.name"
        "      AND (g.expires_at IS NULL OR g.expires_at > unixepoch())"
        " WHERE (?1 IS NULL OR t.name LIKE '%'||?1||'%' OR t.description LIKE '%'||?1||'%')"
        " ORDER BY t.name", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        if (query)
            sqlite3_bind_text(st, 1, query, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(st, 1);
        sqlite3_bind_text(st, 2, ctx->agent_name, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(st, 0);
            const char *desc = (const char *)sqlite3_column_text(st, 1);
            int granted = sqlite3_column_int(st, 2);
            const char *approval = (const char *)sqlite3_column_text(st, 3);
            if (granted) {
                if (approval && (strcmp(approval, "always") == 0 ||
                                 strcmp(approval, "tool_decides") == 0))
                    buf_appendf(&out, "[granted, approval: %s] %s — %s\n",
                                approval, name, desc ? desc : "");
                else
                    buf_appendf(&out, "[granted] %s — %s\n",
                                name, desc ? desc : "");
            } else {
                buf_appendf(&out, "[requestable] %s — %s\n",
                            name, desc ? desc : "");
            }
        }
        sqlite3_finalize(st);
    }

    /* Section 3: global config registry — every key is self-describing
     * (default + description synced from code), so the agent sees the full
     * knob inventory and which values are overrides. */
    buf_appendf(&out, "\n## Global config (value [override|default] — description)\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT key, COALESCE(value, default_value),"
        "       (value IS NOT NULL), COALESCE(description,''), COALESCE(secret,0)"
        " FROM config"
        " WHERE (?1 IS NULL OR key LIKE '%'||?1||'%' OR description LIKE '%'||?1||'%')"
        " ORDER BY key", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        if (query)
            sqlite3_bind_text(st, 1, query, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(st, 1);
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *key = (const char *)sqlite3_column_text(st, 0);
            const char *val = (const char *)sqlite3_column_text(st, 1);
            int overridden = sqlite3_column_int(st, 2);
            const char *desc = (const char *)sqlite3_column_text(st, 3);
            if (sqlite3_column_int(st, 4)) {
                buf_appendf(&out, "%s = (secret, env/secret-store only) — %s\n",
                            key, desc);
                continue;
            }
            buf_appendf(&out, "%s = %s [%s] — %s\n", key, val ? val : "",
                        overridden ? "override" : "default", desc);
        }
        sqlite3_finalize(st);
    }

    /* Section 4: attached extensions — what this agent has loaded. */
    buf_appendf(&out, "\n## Your extensions (attached)\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT e.name, ae.enabled, COALESCE(e.published,0)"
        " FROM agent_extensions ae JOIN extensions e ON e.name=ae.extension_name"
        " WHERE ae.agent_name=?1 ORDER BY e.name", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
        int any = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            any = 1;
            buf_appendf(&out, "%s [%s%s]\n",
                        sqlite3_column_text(st, 0),
                        sqlite3_column_int(st, 1) ? "enabled" : "disabled",
                        sqlite3_column_int(st, 2) ? ", published" : "");
        }
        sqlite3_finalize(st);
        if (!any) buf_appendf(&out, "(none)\n");
    }

    /* Section 5: agent roster — who exists, for launch_agent/create_agent
     * decisions (check before creating a duplicate). */
    buf_appendf(&out, "\n## Agent roster\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT name, sandbox_profile, COALESCE(description,'')"
        " FROM agents ORDER BY name", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            buf_appendf(&out, "%s [%s] — %s\n",
                        sqlite3_column_text(st, 0),
                        sqlite3_column_text(st, 1),
                        sqlite3_column_text(st, 2));
        }
        sqlite3_finalize(st);
    }

    /* Section 6: usage hint */
    buf_appendf(&out,
        "\n## Requesting changes (use the request_config tool)\n"
        "One request_changes document batches everything — one approval covers it all:\n"
        "  {\"action\":\"request_changes\",\"changes\":{\n"
        "    \"grants\":{\"tools\":[\"<name>\"],\"hosts\":[\"<hostname>\"],"
        "\"read_paths\":[\"/abs/path\"],\"write_paths\":[\"/abs/path\"]},\n"
        "    \"config\":{\"<key>\":\"<value>\"}}}\n"
        "(any subset; config keys must be registered keys listed above)\n"
        "- rename agent: {\"action\":\"rename_agent\",\"name\":\"<new_name>\"}\n"
        "Add an optional \"reason\" field — it is shown to the human approver.\n"
        "All gated actions require human approval before taking effect.\n");

    free(query);
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
        "Discover your current configuration and what you can request: your sandbox profile, "
        "granted tools and hosts, the full list of available tools, and how to request more "
        "via request_config. Optional 'query' filters the tool list.",
        PARAMS_JSON, handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "search_config",
                         (ToolRecipe){EXEC_THREAD, SBX_NONE, search_config_thread_run});
    return rc;
}
