/* request_config tool — handled inline by parent process.
 * Actions: grant_tool, grant_host, rename_agent. */
#define _POSIX_C_SOURCE 200809L
#include "tool_request_config.h"
#include "agent_config.h"
#include "db.h"
#include "validate.h"
#include "tool_parse.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"grant_tool\",\"grant_host\",\"rename_agent\"],"
    "\"description\":\"Type of config request\"},"
    "\"tool\":{\"type\":\"string\",\"description\":\"Tool name to enable (for grant_tool)\"},"
    "\"host\":{\"type\":\"string\",\"description\":\"Hostname to allow (for grant_host)\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"New agent name (for rename_agent)\"},"
    "\"preamble\":{\"type\":\"string\",\"description\":\"New system prompt preamble (for rename_agent, optional)\"}"
    "},\"required\":[\"action\"]}";

static char *handler(const char *arguments, void *user_data) {
    RequestConfigCtx *ctx = (RequestConfigCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->agent_name)
        return strdup("error: request_config unavailable");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON");

    const char *act = targ_str(&ta, "action");
    if (!act) { tool_parse_free(&ta); return strdup("error: 'action' required"); }

    if (strcmp(act, "grant_tool") == 0) {
        const char *tool = targ_str(&ta, "tool");
        if (!tool || !tool[0]) { tool_parse_free(&ta); return strdup("error: 'tool' required"); }
        int rc = agent_config_add_tool(ctx->db, ctx->agent_name, tool);
        tool_parse_free(&ta);
        if (rc == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "granted tool: %s", tool);
            return strdup(buf);
        }
        return strdup("error: failed to grant tool");

    } else if (strcmp(act, "grant_host") == 0) {
        const char *host = targ_str(&ta, "host");
        if (!host || !host[0]) { tool_parse_free(&ta); return strdup("error: 'host' required"); }
        int rc = agent_config_add_host(ctx->db, ctx->agent_name, host);
        tool_parse_free(&ta);
        if (rc == 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "granted host: %s", host);
            return strdup(buf);
        }
        return strdup("error: failed to grant host");

    } else if (strcmp(act, "rename_agent") == 0) {
        const char *new_name = targ_str(&ta, "name");
        const char *preamble = targ_str(&ta, "preamble");
        if (!new_name || !new_name[0]) { tool_parse_free(&ta); return strdup("error: 'name' required"); }

        /* Copy strings before freeing parser */
        char *nn = strdup(new_name);
        char *pr = preamble ? strdup(preamble) : NULL;
        tool_parse_free(&ta);

        int rc = agent_rename(ctx->db, ctx->agent_name, nn, ctx->session_id);
        if (rc != 0) {
            const char *msg = rc == -1 ? "error: agent has active sessions"
                            : rc == -2 ? "error: name already taken"
                            : rc == -3 ? "error: invalid name (use A-Za-z0-9_- only)"
                            : "error: agent not found";
            free(nn); free(pr);
            return strdup(msg);
        }

        /* Disk rename */
        if (ctx->agents_dir) {
            char old_path[512], new_path[512];
            snprintf(old_path, sizeof(old_path), "%s/%s", ctx->agents_dir, ctx->agent_name);
            snprintf(new_path, sizeof(new_path), "%s/%s", ctx->agents_dir, nn);
            struct stat st;
            if (stat(old_path, &st) == 0) {
                if (rename(old_path, new_path) != 0) {
                    /* Compensate: rollback DB rename */
                    agent_rename(ctx->db, nn, ctx->agent_name, ctx->session_id);
                    char buf[128];
                    snprintf(buf, sizeof(buf), "error: disk rename failed: %s", strerror(errno));
                    free(nn); free(pr);
                    return strdup(buf);
                }
            }
        }

        /* Optional preamble update */
        if (pr && pr[0]) {
            const char *sql = "UPDATE agents SET system_prompt=? WHERE name=?";
            sqlite3_stmt *s;
            if (sqlite3_prepare_v2(ctx->db, sql, -1, &s, NULL) == SQLITE_OK) {
                sqlite3_bind_text(s, 1, pr, -1, SQLITE_STATIC);
                sqlite3_bind_text(s, 2, nn, -1, SQLITE_STATIC);
                sqlite3_step(s); sqlite3_finalize(s);
            }
        }

        /* Update ctx for subsequent tool calls in this turn */
        snprintf((char *)ctx->agent_name, 64, "%s", nn);
        setenv("CCLAW_AGENT_NAME", nn, 1);

        char buf[128];
        snprintf(buf, sizeof(buf), "agent renamed: %s", nn);
        free(nn); free(pr);
        return strdup(buf);

    } else {
        tool_parse_free(&ta);
        return strdup("error: action must be grant_tool, grant_host, or rename_agent");
    }
}

int tool_request_config_register(ToolRegistry *reg, RequestConfigCtx *ctx) {
    return tools_register(reg, "request_config",
        "Request a configuration change. "
        "Actions: grant_tool (enable shell_exec, web_fetch, db_query), "
        "grant_host (add hostname to allowed_hosts), "
        "rename_agent (rename this agent, with optional preamble).",
        PARAMS_JSON, handler, ctx);
}
