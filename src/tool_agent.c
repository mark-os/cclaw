#define _POSIX_C_SOURCE 200809L
#include "tool_agent.h"
#include "tool_parse.h"
#include "db.h"
#include "wake.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SPAWN_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"task\":{\"type\":\"string\",\"description\":\"Task description for the sub-agent\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"Name of existing agent to launch (omit for default)\"},"
    "\"background\":{\"type\":\"boolean\",\"description\":\"Run in background (default: false, blocks until done)\"}"
    "},\"required\":[\"task\"]}";

int agent_max_depth(sqlite3 *db) {
    char *v = db_kv_get(db, "agent_max_depth");
    if (!v) return AGENT_MAX_DEPTH;
    int d = atoi(v);
    free(v);
    return (d > 0) ? d : AGENT_MAX_DEPTH;
}
char *tool_launch_agent_handler(const char *arguments, void *user_data) {
    AgentLaunchCtx *ctx = (AgentLaunchCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: launch_agent not configured");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *task = targ_str(&ta, "task");
    if (!task || !task[0]) {
        tool_parse_free(&ta);
        return strdup("error: missing or empty 'task' field");
    }

    /* Depth + concurrency checks */
    int depth = session_get_depth(ctx->db, ctx->session_id);
    if (depth >= agent_max_depth(ctx->db)) {
        tool_parse_free(&ta); return strdup("error: max agent depth reached");
    }
    int children = session_count_children(ctx->db, ctx->session_id);
    if (children >= AGENT_MAX_PER_PARENT) {
        tool_parse_free(&ta); return strdup("error: max sub-agents per parent reached");
    }
    int total = session_count_active_agents(ctx->db);
    if (total >= AGENT_MAX_TOTAL) {
        tool_parse_free(&ta); return strdup("error: max system-wide agents reached");
    }

    /* Create child session */
    const char *agent = targ_str(&ta, "name");
    if (!agent || !agent[0]) agent = "default";
    int background = targ_bool(&ta, "background", 0);

    AgentRow *row = db_agent_get(ctx->db, agent);
    if (!row) {
        char err[128];
        snprintf(err, sizeof(err), "error: unknown agent '%.64s'", agent);
        tool_parse_free(&ta);
        return strdup(err);
    }
    agent_row_free(row);

    int64_t child_sid = session_create(ctx->db, "agent", agent,
                                       ctx->session_id, depth + 1);
    if (child_sid < 0) {
        tool_parse_free(&ta);
        return strdup("error: failed to create child session");
    }

    /* If blocking: store parent's tool_call_id on child so completion
     * can write the tool result directly */
    if (!background && ctx->current_tool_call_id) {
        session_set_parent_tool_call_id(ctx->db, child_sid, ctx->current_tool_call_id);
        /* Park parent in waiting state */
        session_set_state(ctx->db, ctx->session_id, "waiting");
    }

    /* Insert task into child's inbox and wake it */
    inbox_insert(ctx->db, child_sid, "spawn", task);
    tool_parse_free(&ta);
    wake_session(child_sid);

    if (background) {
        char buf[128];
        snprintf(buf, sizeof(buf),
                 "agent delegated in background (session_id=%lld). "
                 "Result will arrive in your inbox when done.",
                 (long long)child_sid);
        return strdup(buf);
    }

    /* Blocking: return empty string — real result arrives when child finishes.
     * The tool_call stays pending; advance_session on the child's completion
     * will write the actual result. */
    return NULL;
}

int tool_launch_agent_register(ToolRegistry *reg, AgentLaunchCtx *ctx) {
    return tools_register(reg, "launch_agent",
                          "Delegate a task to another agent",
                          SPAWN_PARAMS_JSON, tool_launch_agent_handler, ctx);
}

/* --- check_agent tool --- */

static const char *CHECK_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"session_id\":{\"type\":\"integer\",\"description\":\"Session ID of the sub-agent to check\"}"
    "},\"required\":[\"session_id\"]}";

char *tool_check_agent_handler(const char *arguments, void *user_data) {
    AgentLaunchCtx *ctx = (AgentLaunchCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: check_agent not configured");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    int sid_val = targ_int(&ta, "session_id", -1);
    tool_parse_free(&ta);
    if (sid_val < 0) return strdup("error: missing session_id");
    int64_t child_sid = (int64_t)sid_val;

    /* Verify it's our child */
    const char *sql = "SELECT state FROM sessions WHERE id=? AND parent_session_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return strdup("error: db query failed");
    sqlite3_bind_int64(stmt, 1, child_sid);
    sqlite3_bind_int64(stmt, 2, ctx->session_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return strdup("error: session not found or not a child");
    }
    const char *state = (const char *)sqlite3_column_text(stmt, 0);
    char state_buf[32];
    snprintf(state_buf, sizeof(state_buf), "%s", state ? state : "unknown");
    sqlite3_finalize(stmt);

    /* Plain text output */
    char *result = NULL;
    if (strcmp(state_buf, "idle") == 0)
        result = get_response_text(ctx->db, child_sid);

    size_t needed = 128 + (result ? strlen(result) : 0);
    char *out = malloc(needed);
    if (!out) { free(result); return strdup("error: OOM"); }
    if (result) {
        snprintf(out, needed, "session_id: %lld\nstate: %s\nresult: %s",
                 (long long)child_sid, state_buf, result);
        free(result);
    } else {
        snprintf(out, needed, "session_id: %lld\nstate: %s",
                 (long long)child_sid, state_buf);
    }
    return out;
}

int tool_check_agent_register(ToolRegistry *reg, AgentLaunchCtx *ctx) {
    return tools_register(reg, "check_agent",
                          "Check the status and result of a sub-agent",
                          CHECK_PARAMS_JSON, tool_check_agent_handler, ctx);
}
