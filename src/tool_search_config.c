/* search_config tool — read-only introspection of agent config and available
 * tools, in two renderings of one query layer: prose (default) and the
 * canonical agent document (format:json).
 *
 * The json rendering is the read half of the changes-doc section names
 * (agent, grants, routes, config, provider(s), models, secret_bindings) that
 * request_config patches — read-shape ≈ write-shape on purpose. The other
 * three consumers are park/validate + apply (src/tool_request_config.c) and
 * the approval card (src/approval.c). Contract: specs/config-doc.md. */
#define _POSIX_C_SOURCE 200809L
#include "tool_search_config.h"
#include "approval.h"
#include "tool_args.h"
#include "buf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"query\":{\"type\":\"string\",\"description\":\"Optional substring to filter tools by name/description\"},"
    "\"format\":{\"type\":\"string\",\"enum\":[\"text\",\"json\"],"
    "\"description\":\"'text' (default, prose) or 'json' — the canonical agent "
    "document, whose section names are the ones request_config patches\"}"
    "}}";

/* Open approvals on this session as a JSON array of
 * {id, age_seconds, age, summary} — built once and rendered by BOTH formats,
 * so the two views cannot drift. Pending-only, matching the <open_approvals>
 * context block (src/llm_payload.c): "waiting on a human" is the one approval
 * state the model can act on. Never NULL on success; '[]' when there are none. */
static char *pending_approvals_json(sqlite3 *db, int64_t session_id) {
    char *arr = strdup("[]");
    sqlite3_stmt *st;
    if (!arr) return NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id, tool_call_id, tool_name, park_reason, args_json, resolve,"
            "       MAX(unixepoch() - requested_at, 0)"
            "  FROM approvals WHERE session_id=?1 AND state='pending'"
            " ORDER BY id", -1, &st, NULL) != SQLITE_OK)
        return arr;
    sqlite3_bind_int64(st, 1, session_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        /* Borrowed column pointers, valid until the next step — the summary
         * renderer only reads, so no copy is warranted. */
        Approval a = {0};
        a.id = sqlite3_column_int64(st, 0);
        a.session_id = session_id;
        a.tool_call_id = (char *)sqlite3_column_text(st, 1);
        a.tool_name = (char *)sqlite3_column_text(st, 2);
        a.park_reason = (char *)sqlite3_column_text(st, 3);
        a.args_json = (char *)sqlite3_column_text(st, 4);
        a.resolve = (char *)sqlite3_column_text(st, 5);
        int64_t age = sqlite3_column_int64(st, 6);
        char *summary = approval_format_summary(db, &a);
        sqlite3_stmt *ap;
        if (sqlite3_prepare_v2(db,
                "SELECT json_insert(?1,'$[#]', json_object("
                "  'id', ?2, 'age_seconds', ?3,"
                "  'age', CASE WHEN ?3 < 60 THEN ?3 || 's'"
                "              WHEN ?3 < 3600 THEN (?3/60) || 'm'"
                "              ELSE (?3/3600) || 'h' END,"
                "  'summary', ?4))", -1, &ap, NULL) == SQLITE_OK) {
            sqlite3_bind_text(ap, 1, arr, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ap, 2, a.id);
            sqlite3_bind_int64(ap, 3, age);
            sqlite3_bind_text(ap, 4, summary ? summary : "(no summary)", -1,
                              SQLITE_STATIC);
            if (sqlite3_step(ap) == SQLITE_ROW) {
                const char *v = (const char *)sqlite3_column_text(ap, 0);
                if (v) { char *n = strdup(v); if (n) { free(arr); arr = n; } }
            }
            sqlite3_finalize(ap);
        }
        free(summary);
    }
    sqlite3_finalize(st);
    return arr;
}

/* The canonical agent document, assembled by SQL (the repo's serializer — see
 * src/llm_payload.c). Binds: ?1 agent, ?2 session id, ?3 shell-path env
 * override, ?4 filter substring (NULL = none), ?5 pending-approvals array.
 * Every scalar subquery is json()-wrapped: a subquery result loses SQLite's
 * JSON subtype, and an unwrapped one would embed as a quoted string. */
static const char SQL_CONFIG_DOC[] =
    "SELECT json_object("
    " 'agent', json_object("
    "   'name', ?1,"
    "   'sandbox_profile', (SELECT sandbox_profile FROM agents WHERE name=?1),"
    /* An entry with a reasoning effort shows as {id, effort} — the same union
     * shape request_config accepts, so the doc round-trips as a patch base. */
    "   'models', json((SELECT json_group_array(json("
    "     CASE WHEN reasoning_effort IS NULL THEN json_quote(model_id)"
    "          ELSE json_object('id', model_id, 'effort', reasoning_effort)"
    "     END)) FROM (SELECT model_id, reasoning_effort FROM agent_models"
    "                  WHERE agent_name=?1 ORDER BY pos))),"
    "   'max_iterations', (SELECT max_iterations FROM agents WHERE name=?1),"
    "   'shell_timeout', (SELECT shell_timeout FROM agents WHERE name=?1),"
    "   'shell_path', (SELECT COALESCE(NULLIF(?3,''),NULLIF(shell_path,''),"
    "                                  '/bin/sh') FROM agents WHERE name=?1)),"
    " 'grants', json_object("
    "   'tools', json((SELECT json_group_array(value) FROM grants"
    "     WHERE agent_name=?1 AND kind='tool'"
    "       AND (expires_at IS NULL OR expires_at>unixepoch()))),"
    "   'hosts', json((SELECT json_group_array(value) FROM grants"
    "     WHERE agent_name=?1 AND kind='host'"
    "       AND (expires_at IS NULL OR expires_at>unixepoch()))),"
    "   'read_paths', json((SELECT json_group_array(value) FROM grants"
    "     WHERE agent_name=?1 AND kind='read_path'"
    "       AND (expires_at IS NULL OR expires_at>unixepoch()))),"
    "   'write_paths', json((SELECT json_group_array(value) FROM grants"
    "     WHERE agent_name=?1 AND kind='write_path'"
    "       AND (expires_at IS NULL OR expires_at>unixepoch())))),"
    " 'session', json_object('id', ?2,"
    "   'tool_filter', json((SELECT COALESCE(tool_filter,'null') FROM sessions"
    "                        WHERE id=?2))),"
    " 'sensitive_hosts', json((SELECT json_group_array(value)"
    "   FROM sensitive_targets WHERE kind='host')),"
    " 'secret_bindings', json((SELECT COALESCE(json_group_object(secret_name,"
    "     json(hosts)),'{}') FROM (SELECT secret_name,"
    "       json_group_array(host) AS hosts FROM secret_hosts"
    "       GROUP BY secret_name))),"
    " 'providers', json((SELECT json_group_array(json_object("
    "     'name', name, 'endpoint_type', endpoint_type, 'base_url', base_url,"
    "     'api_key_env', api_key_env)) FROM (SELECT * FROM providers"
    "     ORDER BY name))),"
    " 'models', json((SELECT json_group_array(json_object("
    "     'id', id, 'status', status, 'context_window', context_window,"
    "     'your_position', pos)) FROM ("
    "   SELECT m.id, m.status, m.context_window,"
    "          (SELECT am.pos+1 FROM agent_models am"
    "            WHERE am.agent_name=?1 AND am.model_id=m.id) AS pos"
    "     FROM models m WHERE (?4 IS NULL OR m.id LIKE '%'||?4||'%')"
    "    ORDER BY m.created_at, m.id))),"
    " 'tools', json((SELECT json_group_array(json_object("
    "     'name', name, 'granted', granted, 'approval_mode', approval_mode))"
    "   FROM (SELECT t.name AS name,"
    "                (g.agent_name IS NOT NULL) AS granted,"
    "                g.approval_mode AS approval_mode"
    "           FROM tools t"
    "           LEFT JOIN grants g ON g.agent_name=?1 AND g.kind='tool'"
    "                AND g.value=t.name"
    "                AND (g.expires_at IS NULL OR g.expires_at>unixepoch())"
    "          WHERE t.enabled=1 AND (t.agent_name IS NULL OR t.agent_name=?1)"
    "            AND (?4 IS NULL OR t.name LIKE '%'||?4||'%'"
    "                 OR t.description LIKE '%'||?4||'%')"
    "          ORDER BY t.name))),"
    " 'config', json((SELECT COALESCE(json_group_object(key, json_object("
    "     'value', CASE WHEN COALESCE(secret,0) THEN NULL"
    "                   ELSE COALESCE(value, default_value) END,"
    "     'overridden', value IS NOT NULL,"
    "     'secret', COALESCE(secret,0))),'{}') FROM config"
    "   WHERE (?4 IS NULL OR key LIKE '%'||?4||'%'"
    "          OR description LIKE '%'||?4||'%'))),"
    " 'extensions', json((SELECT json_group_array(json_object("
    "     'name', e.name, 'enabled', ae.enabled))"
    "   FROM agent_extensions ae JOIN extensions e ON e.name=ae.extension_name"
    "   WHERE ae.agent_name=?1)),"
    " 'agents', json((SELECT json_group_array(json_object("
    "     'name', name, 'sandbox_profile', sandbox_profile))"
    "   FROM (SELECT * FROM agents ORDER BY name))),"
    " 'pending_approvals', json(?5))";

/* format:json — one document, same data the prose view renders. */
static char *render_json(SearchConfigCtx *ctx, const char *query,
                         const char *pending, int *is_error) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(ctx->db, SQL_CONFIG_DOC, -1, &st, NULL) != SQLITE_OK)
        return tool_fail(is_error, "error: could not build the config document");
    const char *env_shell = getenv("CCLAW_SHELL_PATH");
    sqlite3_bind_text(st, 1, ctx->agent_name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, ctx->session_id);
    sqlite3_bind_text(st, 3, env_shell ? env_shell : "", -1, SQLITE_STATIC);
    if (query) sqlite3_bind_text(st, 4, query, -1, SQLITE_STATIC);
    else sqlite3_bind_null(st, 4);
    sqlite3_bind_text(st, 5, pending, -1, SQLITE_STATIC);
    char *doc = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) doc = strdup(v);
    }
    sqlite3_finalize(st);
    return doc ? doc
               : tool_fail(is_error, "error: could not build the config document");
}

/* Pending approvals, prose form — rendered from the same JSON array the json
 * format returns. Summaries are markdown paragraphs; flattened to one line and
 * clipped so a long parked document cannot bury the rest of the output. */
static void append_pending_text(sqlite3 *db, Buf *out, const char *pending) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT group_concat('#' || json_extract(value,'$.id') ||"
            "  ' (' || json_extract(value,'$.age') || ' old) ' ||"
            "  substr(replace(json_extract(value,'$.summary'),char(10),' '),1,200),"
            "  char(10)) FROM json_each(?1)", -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, pending, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        buf_appendf(out, "\n## Pending approvals (this session)\n%s\n",
                    (v && v[0]) ? v : "(none — nothing is waiting on a human)");
    }
    sqlite3_finalize(st);
}

static char *render_text(SearchConfigCtx *ctx, const char *query,
                         const char *pending, int *is_error) {
    Buf out = {0};

    /* Section 0: the agent's own settings row — "what is my config now" must
     * be answerable here (validate_agent's error refers agents to this tool).
     * shell_path comes from the DB, not CCLAW_SHELL_PATH: the env var is
     * process-global and holds whichever agent dispatched last. The env
     * override, when set, applies to every agent equally, so show it. */
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(ctx->db,
        "SELECT COALESCE((SELECT group_concat(m, ' > ')"
        "                 FROM (SELECT model_id"
        "                         ||COALESCE(' (effort '||reasoning_effort||')','') m"
        "                       FROM agent_models"
        "                       WHERE agent_name=?1 ORDER BY pos)),"
        "                '(none — unroutable)'),"
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
                "models: %s\n"
                "max_iterations: %lld\n"
                "shell_timeout: %lld\n"
                "shell_path: %s\n",
                ctx->agent_name,
                sqlite3_column_text(st, 0),
                (long long)sqlite3_column_int64(st, 1),
                (long long)sqlite3_column_int64(st, 2),
                sqlite3_column_text(st, 3));
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

    /* Registered models: the valid values for an agent's routing list.
     * Marks the caller's list position so "what am I running" is a one-call
     * lookup. */
    buf_appendf(&out, "\n## Registered models\n");
    rc = sqlite3_prepare_v2(ctx->db,
        "SELECT m.id, m.status, COALESCE(m.context_window,0),"
        "       (SELECT pos FROM agent_models am"
        "         WHERE am.agent_name=?2 AND am.model_id=m.id)"
        " FROM models m"
        " WHERE (?1 IS NULL OR m.id LIKE '%'||?1||'%')"
        " ORDER BY m.created_at, m.id", -1, &st, NULL);
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
            if (sqlite3_column_type(st, 3) != SQLITE_NULL)
                buf_appendf(&out, " <- your list #%d", sqlite3_column_int(st, 3) + 1);
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
        "One changes document batches everything — one approval covers it all.\n"
        "It is a PATCH against the state shown above: agent.* fields replace;\n"
        "grants.* add; models/provider upsert; grants.remove narrows immediately\n"
        "without approval.\n"
        "  {\"changes\":{\n"
        "    \"grants\":{\"tools\":[\"<name>\"],\"hosts\":[\"<hostname>\"],"
        "\"read_paths\":[\"/abs/path\"],\"write_paths\":[\"/abs/path\"],\n"
        "              \"remove\":{\"hosts\":[\"<hostname you already have>\"]}},\n"
        "    \"agent\":{\"models\":[\"<model@provider>\", \"...\"],"
        "\"max_iterations\":<n>,\"shell_timeout\":<n>},\n"
        "    \"routes\":[\"<channel>:<chat_id>\"],\n"
        "    \"config\":{\"<key>\":\"<value>\"},\n"
        "    \"provider\":{\"provider\":\"<name>\"}}}\n"
        "(any subset; grants/agent/routes change only you, config/provider are\n"
        "system-wide; config keys must be registered keys listed above)\n"
        "Add an optional \"reason\" field — it is shown to the human approver.\n"
        "Everything except grants.remove requires human approval before taking"
        " effect.\n");

    append_pending_text(ctx->db, &out, pending);

    char *result = buf_take(&out);
    return result ? result : tool_fail(is_error, "error: out of memory");
}

static char *handler(const char *arguments, void *user_data, int *is_error) {
    SearchConfigCtx *ctx = (SearchConfigCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return tool_fail(is_error, "error: search_config unavailable");

    char *query = tool_args_str(ctx->db, arguments, "query");
    if (query && !query[0]) { free(query); query = NULL; }  /* "" = no filter */
    char *format = tool_args_str(ctx->db, arguments, "format");
    int want_json = format && strcmp(format, "json") == 0;
    char *bad = (format && format[0] && !want_json &&
                 strcmp(format, "text") != 0)
        ? tool_fail(is_error, "error: format must be 'text' or 'json'") : NULL;

    char *result = bad;
    if (!result) {
        char *pending = pending_approvals_json(ctx->db, ctx->session_id);
        if (!pending)
            result = tool_fail(is_error, "error: out of memory");
        else
            result = want_json ? render_json(ctx, query, pending, is_error)
                               : render_text(ctx, query, pending, is_error);
        free(pending);
    }
    free(query);
    free(format);
    return result;
}

/* EXEC_THREAD shim: rebuild SearchConfigCtx around the thread's own db. */
static char *search_config_thread_run(sqlite3 *db, const char *agent_name,
                                      int64_t session_id, const char *args,
                                      int *is_error) {
    SearchConfigCtx c = {.db = db, .agent_name = agent_name,
                         .session_id = session_id};
    return handler(args, &c, is_error);
}

int tool_search_config_register(ToolRegistry *reg, SearchConfigCtx *ctx) {
    int rc = tools_register(reg, "search_config",
        "Discover your current configuration and what you can request: your sandbox profile, "
        "granted tools and hosts, the full list of available tools, pending approvals, and "
        "how to request more via request_config. Optional 'query' filters the tool list; "
        "optional 'format':'json' returns the same state as one canonical document whose "
        "sections are the ones a request_config changes document patches.",
        PARAMS_JSON, handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "search_config",
                         (ToolRecipe){.vehicle = EXEC_THREAD, .thread_run = search_config_thread_run});
    return rc;
}
