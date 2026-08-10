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

    /* Section 0: the agent's own settings row — "what is my config now" must
     * be answerable here (validate_agent's error refers agents to this tool).
     * shell_path comes from the DB, not CCLAW_SHELL_PATH: the env var is
     * process-global and holds whichever agent dispatched last. The env
     * override, when set, applies to every agent equally, so show it. */
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(ctx->db,
        "SELECT COALESCE(NULLIF(primary_model,''),'(system default)'),"
        "       COALESCE(NULLIF(secondary_model,''),'(none)'),"
        "       max_iterations, shell_timeout,"
        "       COALESCE(NULLIF(?2,''),NULLIF(shell_path,''),'/bin/sh')"
        " FROM agents WHERE name=?1", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        const char *env_shell = getenv("CCLAW_SHELL_PATH");
        sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, env_shell ? env_shell : "", -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            buf_appendf(&out,
                "## Your settings (agent: %s)\n"
                "primary_model: %s\n"
                "secondary_model: %s\n"
                "max_iterations: %lld\n"
                "shell_timeout: %lld\n"
                "shell_path: %s\n",
                ctx->agent_name,
                sqlite3_column_text(st, 0),
                sqlite3_column_text(st, 1),
                (long long)sqlite3_column_int64(st, 2),
                (long long)sqlite3_column_int64(st, 3),
                sqlite3_column_text(st, 4));
        }
        sqlite3_finalize(st);
    }

    /* Section 1: current grants */
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT sandbox_profile FROM agents WHERE name=?1", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            buf_appendf(&out,
                "\n## Your current grants (agent: %s)\n"
                "sandbox_profile: %s\n",
                ctx->agent_name, sqlite3_column_text(st, 0));
        }
        sqlite3_finalize(st);
    }

    /* Report grants per kind */
    static const char *kinds[] = {"host", "tool", "read_path", "write_path"};
    for (int ki = 0; ki < 4; ki++) {
        rc = sqlite3_prepare_v2(ctx->db,
            "SELECT COALESCE(group_concat(value, ', '), '(none)')"
            " FROM grants WHERE agent_name=?1 AND kind=?2"
            " AND (expires_at IS NULL OR expires_at > unixepoch())",
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

    /* Session tool_filter: grants are the agent's authority, but a worker
     * session is spawned with a narrowed toolset that intersects them. Without
     * this line a filtered worker reads "[granted] launch_agent" and cannot
     * tell why the call is refused (observed, CharlesDow 2026-07-31). */
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT (SELECT group_concat(value, ', ') FROM json_each(s.tool_filter))"
        " FROM sessions s WHERE s.id=?1 AND s.tool_filter IS NOT NULL",
        -1, &st, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, ctx->session_id);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            buf_appendf(&out,
                "session tool filter: %s (THIS session only — your effective"
                " tools are the grants above intersected with this list;"
                " whoever spawned this session chose it)\n",
                (v && v[0]) ? v : "(empty — no tools callable)");
        }
        sqlite3_finalize(st);
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
     * A {{SECRET:X}} aimed anywhere else is denied with the missing pair
     * named; the binding is requested via request_config. */
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT COALESCE(group_concat(secret_name || '->' || host, ', '), '(none)')"
        " FROM secret_hosts", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *v = (const char *)sqlite3_column_text(st, 0);
            buf_appendf(&out,
                "secret_bindings: %s (a secret submitted to an unbound host is"
                " denied — request the pair via request_config"
                " secret_bindings)\n", v ? v : "(none)");
        }
        sqlite3_finalize(st);
    }

    /* Providers: transport config only — endpoint + protocol, never key
     * material (api_key_env is a secret NAME; the value stays encrypted). */
    buf_appendf(&out, "\n## Providers\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT name, endpoint_type, base_url,"
        "       COALESCE(NULLIF(default_model,''),'(none)')"
        " FROM providers ORDER BY name", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        int any = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            any = 1;
            buf_appendf(&out, "%s [%s] %s (default_model: %s)\n",
                        sqlite3_column_text(st, 0), sqlite3_column_text(st, 1),
                        sqlite3_column_text(st, 2), sqlite3_column_text(st, 3));
        }
        sqlite3_finalize(st);
        if (!any) buf_appendf(&out, "(none)\n");
    }

    /* Registered models: the valid values for agent.primary_model. Marks the
     * caller's active one so "what am I running" is a one-call lookup. */
    buf_appendf(&out, "\n## Registered models\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT m.id, m.status, COALESCE(m.context_window,0),"
        "       (m.id=a.primary_model OR m.model=a.primary_model) AS is_primary,"
        "       (m.id=a.secondary_model OR m.model=a.secondary_model) AS is_secondary"
        " FROM models m, agents a"
        " WHERE a.name=?2"
        "   AND (?1 IS NULL OR m.id LIKE '%'||?1||'%')"
        " ORDER BY m.priority, m.id", -1, &st, NULL);
    if (rc == SQLITE_OK) {
        int any = 0;
        if (query)
            sqlite3_bind_text(st, 1, query, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(st, 1);
        sqlite3_bind_text(st, 2, ctx->agent_name, -1, SQLITE_STATIC);
        while (sqlite3_step(st) == SQLITE_ROW) {
            any = 1;
            long long cw = sqlite3_column_int64(st, 2);
            buf_appendf(&out, "%s [%s]", sqlite3_column_text(st, 0),
                        sqlite3_column_text(st, 1));
            if (cw > 0) buf_appendf(&out, " ctx:%lld", cw);
            if (sqlite3_column_int(st, 3)) buf_appendf(&out, " <- your primary");
            if (sqlite3_column_int(st, 4)) buf_appendf(&out, " <- your secondary");
            buf_appendf(&out, "\n");
        }
        sqlite3_finalize(st);
        if (!any)
            buf_appendf(&out, query ? "(no registered model matches the query)\n"
                                    : "(none)\n");
    }

    /* Section 2: tools list with grant status. Scope like the system prompt's
     * tool section (agent_config.c): disabled or other-agent tools are not
     * requestable — showing them invites approvals on tools that can never
     * load for this agent. */
    buf_appendf(&out, "\n## Tools you can use or request\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT t.name, t.description,"
        "       (g.agent_name IS NOT NULL) AS granted,"
        "       g.approval_mode"
        " FROM tools t"
        " LEFT JOIN grants g ON g.agent_name=?2 AND g.kind='tool' AND g.value=t.name"
        "      AND (g.expires_at IS NULL OR g.expires_at > unixepoch())"
        " WHERE t.enabled=1 AND (t.agent_name IS NULL OR t.agent_name=?2)"
        " AND (?1 IS NULL OR t.name LIKE '%'||?1||'%' OR t.description LIKE '%'||?1||'%')"
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
        "    \"agent\":{\"primary_model\":\"<model[@provider]>\","
        "\"max_iterations\":<n>,\"shell_timeout\":<n>},\n"
        "    \"routes\":[\"<channel>:<chat_id>\"],\n"
        "    \"config\":{\"<key>\":\"<value>\"},\n"
        "    \"provider\":{\"provider\":\"<name>\"}}}\n"
        "(any subset; grants/agent/routes change only you, config/provider are\n"
        "system-wide; config keys must be registered keys listed above)\n"
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
    SearchConfigCtx c = {.db = db, .agent_name = agent_name,
                         .session_id = session_id};
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
