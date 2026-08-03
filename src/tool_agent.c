#define _POSIX_C_SOURCE 200809L
#include "tool_agent.h"
#include "config_registry.h"
#include "tool_args.h"
#include "db.h"
#include "approval.h"
#include "buf.h"
#include "wake.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SPAWN_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"task\":{\"type\":\"string\",\"description\":\"Task description for the sub-agent\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"Agent to launch — a name from the roster; omit to spawn a copy of yourself (worker)\"},"
    "\"tools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Self-spawn only (error if 'name' is set): restrict the worker to these tools, intersected with your own grants — it can never widen them. Omitted, the worker gets the default worker toolset named at the end of this description.\"},"
    "\"background\":{\"type\":\"boolean\",\"description\":\"Default false: the call blocks and its result is the child's final answer. True returns session_id immediately and the child's result is delivered to you when it finishes — no need to check on it.\"}"
    "},\"required\":[\"task\"]}";

int agent_max_depth(sqlite3 *db) {
    int d = config_get_int(db, "agent_max_depth");
    return (d > 0) ? d : config_default_int("agent_max_depth");
}
char *tool_launch_agent_handler(const char *arguments, void *user_data) {
    AgentLaunchCtx *ctx = (AgentLaunchCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: launch_agent not configured");

    char *task = tool_args_str(ctx->db, arguments, "task");
    if (!task || !task[0]) {
        free(task);
        return strdup("error: missing or empty 'task' field");
    }

    /* The launch gate: depth + per-parent + system-wide existence caps.
     * Refusal is right here — a synchronous caller (the model) is present to
     * hear it — and each error names its knob so the model can report it. */
    int depth = session_get_depth(ctx->db, ctx->session_id);
    if (depth >= agent_max_depth(ctx->db)) {
        free(task); return strdup("error: max agent depth reached");
    }
    char err[128];
    int per_parent = config_get_int(ctx->db, "agent_max_per_parent");
    int children = session_count_children(ctx->db, ctx->session_id);
    if (per_parent > 0 && children >= per_parent) {
        free(task);
        snprintf(err, sizeof(err), "error: max sub-agents per parent reached"
                 " (agent_max_per_parent=%d)", per_parent);
        return strdup(err);
    }
    int max_active = config_get_int(ctx->db, "session_max_active");
    int total = session_count_active_agents(ctx->db);
    if (max_active > 0 && total >= max_active) {
        free(task);
        snprintf(err, sizeof(err), "error: max system-wide agents reached"
                 " (session_max_active=%d)", max_active);
        return strdup(err);
    }

    /* Create child session. Omitted name = self-spawn: a worker running as
     * the calling agent, scoped down by a frozen session tool_filter
     * (explicit `tools` arg → kv worker_tools → unrestricted). The filter is
     * intersection-only against grants — it can never add authority. */
    char *agent = tool_args_str(ctx->db, arguments, "name");
    int self_spawn = (!agent || !agent[0]);
    int background = tool_args_bool(ctx->db, arguments, "background", 0);

    char *tools_json = tool_args_json(ctx->db, arguments, "tools");
    if (tools_json) {
        if (!self_spawn) {
            free(task); free(agent); free(tools_json);
            return strdup("error: 'tools' filter is only valid for self-spawn (omit 'name')");
        }
        if (tools_json[0] != '[') {
            free(task); free(agent); free(tools_json);
            return strdup("error: 'tools' must be a JSON array of tool names");
        }
    }

    char *self_name = NULL;
    if (self_spawn) {
        self_name = session_get_agent_name(ctx->db, ctx->session_id);
        if (!self_name) {
            free(task); free(agent); free(tools_json);
            return strdup("error: cannot self-spawn — calling session has no agent");
        }
        free(agent);
        agent = self_name;
        self_name = NULL; /* ownership transferred to agent */
    }

    AgentRow *row = db_agent_get(ctx->db, agent);
    if (!row) {
        char err[128];
        snprintf(err, sizeof(err), "error: unknown agent '%.64s'", agent);
        free(task); free(agent); free(tools_json);
        return strdup(err);
    }
    agent_row_free(row);

    /* Resolve the worker's tool filter, frozen into the session row. */
    char *filter = NULL;
    if (self_spawn) {
        if (tools_json) {
            filter = tools_json;
            tools_json = NULL; /* ownership transferred */
        } else {
            filter = config_get(ctx->db, "worker_tools");
        }
    }
    free(tools_json);

    int64_t child_sid = session_create_filtered(ctx->db, "agent", agent,
                                                ctx->session_id, depth + 1,
                                                filter);
    free(filter);
    if (child_sid < 0) {
        free(task); free(agent);
        return strdup("error: failed to create child session");
    }

    /* If blocking: record which of the parent's tool_calls this child answers.
     * The parent stays in tool_running with that call marked 'running' (by the
     * dispatcher) — it is NOT parked on a single child, so multiple sub-agents
     * can be outstanding at once. On completion advance_session writes the tool
     * result keyed by this id and flips the call to 'done' (see advance.c). */
    if (!background && ctx->current_tool_call_id)
        session_set_parent_tool_call_id(ctx->db, child_sid, ctx->current_tool_call_id);

    /* Insert task into child's inbox and wake it */
    inbox_insert(ctx->db, child_sid, "spawn", NULL, task);
    free(task);
    free(agent);
    wake_session(child_sid);

    if (background) {
        char buf[192];
        snprintf(buf, sizeof(buf),
                 "agent delegated in background (session_id=%lld). "
                 "Its result will be delivered to you when it finishes — "
                 "no need to check on it.",
                 (long long)child_sid);
        return strdup(buf);
    }

    /* Blocking: return NULL — the parent was parked in 'waiting' above and its
     * tool_call stays pending. The dispatcher special-cases this NULL for
     * launch_agent (see main.c, "NULL return means blocking") and skips writing
     * a result; advance_session writes the real result on the child's
     * completion (see advance.c, parent_tool_call_id path). If you make another
     * tool return NULL to park, update the dispatcher's allowlist to match. */
    return NULL;
}

int tool_launch_agent_register(ToolRegistry *reg, AgentLaunchCtx *ctx) {
    int rc = tools_register(reg, "launch_agent",
                          "Delegate a task: start a session on a roster agent, or omit "
                          "'name' to self-spawn a worker with your identity (optionally "
                          "narrowed via 'tools'). Blocks until the child finishes unless "
                          "background:true, which returns a session id and delivers the "
                          "child's result to you when it finishes — you will be notified, "
                          "so do not poll it or schedule a check on it. check_session is a "
                          "live progress peek, not a way to wait. "
                          "Prefer delegating to an existing specialist over doing "
                          "everything inline — the roster is in search_config.",
                          SPAWN_PARAMS_JSON, tool_launch_agent_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "launch_agent", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}

/* --- check_session tool --- */

static const char *CHECK_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"session_id\":{\"type\":\"integer\",\"description\":\"Session ID of the sub-agent to check\"}"
    "},\"required\":[\"session_id\"]}";

char *tool_check_session_handler(const char *arguments, void *user_data) {
    AgentLaunchCtx *ctx = (AgentLaunchCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: check_session not configured");

    int sid_val = tool_args_int(ctx->db, arguments, "session_id", -1);
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
    if (strcmp(state_buf, "idle") == 0) {
        result = get_response_text(ctx->db, child_sid);
        /* Consumed by poll: one column serves push and poll, so the convergence
         * sweep never re-pushes a result the parent already collected here. */
        sqlite3_stmt *up;
        if (sqlite3_prepare_v2(ctx->db,
                "UPDATE sessions SET parent_notified_at=unixepoch() WHERE id=?",
                -1, &up, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(up, 1, child_sid);
            sqlite3_step(up);
            sqlite3_finalize(up);
        }
    }

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

int tool_check_session_register(ToolRegistry *reg, AgentLaunchCtx *ctx) {
    int rc = tools_register(reg, "check_session",
                          "Peek at a sub-agent session's live state, plus its final text "
                          "once it is idle. A progress peek, not a way to wait: a "
                          "background result is delivered to you on its own.",
                          CHECK_PARAMS_JSON, tool_check_session_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "check_session", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}


