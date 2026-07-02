#define _POSIX_C_SOURCE 200809L
#include "tool_agent.h"
#include "tool_parse.h"
#include "db.h"
#include "approval.h"
#include "wake.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *SPAWN_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"task\":{\"type\":\"string\",\"description\":\"Task description for the sub-agent\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"Agent to launch — a name from the roster; omit to spawn a copy of yourself (worker)\"},"
    "\"tools\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"Self-spawn only: restrict the worker to these tools (intersected with your grants; default: the worker_tools config)\"},"
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

    /* Create child session. Omitted name = self-spawn: a worker running as
     * the calling agent, scoped down by a frozen session tool_filter
     * (explicit `tools` arg → kv worker_tools → unrestricted). The filter is
     * intersection-only against grants — it can never add authority. */
    const char *agent = targ_str(&ta, "name");
    int self_spawn = (!agent || !agent[0]);
    int background = targ_bool(&ta, "background", 0);

    const char *tools_raw = NULL;
    size_t tools_len = 0;
    if (targ_raw(&ta, "tools", &tools_raw, &tools_len) == 0) {
        if (!self_spawn) {
            tool_parse_free(&ta);
            return strdup("error: 'tools' filter is only valid for self-spawn (omit 'name')");
        }
        if (tools_len == 0 || tools_raw[0] != '[') {
            tool_parse_free(&ta);
            return strdup("error: 'tools' must be a JSON array of tool names");
        }
    }

    char *self_name = NULL;
    if (self_spawn) {
        self_name = session_get_agent_name(ctx->db, ctx->session_id);
        if (!self_name) {
            tool_parse_free(&ta);
            return strdup("error: cannot self-spawn — calling session has no agent");
        }
        agent = self_name;
    }

    AgentRow *row = db_agent_get(ctx->db, agent);
    if (!row) {
        char err[128];
        snprintf(err, sizeof(err), "error: unknown agent '%.64s'", agent);
        free(self_name);
        tool_parse_free(&ta);
        return strdup(err);
    }
    agent_row_free(row);

    /* Resolve the worker's tool filter, frozen into the session row. */
    char *filter = NULL;
    if (self_spawn) {
        if (tools_raw) {
            filter = malloc(tools_len + 1);
            if (filter) { memcpy(filter, tools_raw, tools_len); filter[tools_len] = '\0'; }
        } else {
            filter = db_kv_get(ctx->db, "worker_tools");
        }
    }

    int64_t child_sid = session_create_filtered(ctx->db, "agent", agent,
                                                ctx->session_id, depth + 1,
                                                filter);
    free(filter);
    free(self_name);
    if (child_sid < 0) {
        tool_parse_free(&ta);
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
                          "Delegate a task to another agent",
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

int tool_check_session_register(ToolRegistry *reg, AgentLaunchCtx *ctx) {
    int rc = tools_register(reg, "check_session",
                          "Check the status and result of a sub-agent session",
                          CHECK_PARAMS_JSON, tool_check_session_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "check_session", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}

/* --- check_approval tool ---
 * Lets an agent inspect its own approvals and re-raise a decided one. Reuses
 * AgentLaunchCtx for the {db, session_id} it needs. */

static const char *CHECK_APPROVAL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"action\":{\"type\":\"string\",\"enum\":[\"status\",\"rerequest\"],"
    "\"description\":\"'status' lists this session's approvals; 'rerequest' re-raises a decided one\"},"
    "\"approval_id\":{\"type\":\"integer\",\"description\":\"Approval id (required for 'rerequest')\"}"
    "},\"required\":[\"action\"]}";

char *tool_check_approval_handler(const char *arguments, void *user_data) {
    AgentLaunchCtx *ctx = (AgentLaunchCtx *)user_data;
    if (!ctx || !ctx->db)
        return strdup("error: check_approval not configured");

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *action = targ_str(&ta, "action");
    if (!action || !action[0]) { tool_parse_free(&ta); return strdup("error: missing action"); }

    if (strcmp(action, "status") == 0) {
        int block_sec = 60;
        char *kv = db_kv_get(ctx->db, "approval_block_sec");
        if (kv) { long v = strtol(kv, NULL, 10); if (v > 0) block_sec = (int)v; free(kv); }
        tool_parse_free(&ta);

        const char *sql =
            "SELECT id, COALESCE(tool_name, action, '?'), state, COALESCE(decided_via,''),"
            " requested_at + ?2, COALESCE(expires_at, 0)"
            " FROM approvals WHERE session_id=?1 ORDER BY id DESC LIMIT 20;";
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(ctx->db, sql, -1, &st, NULL) != SQLITE_OK)
            return strdup("error: db query failed");
        sqlite3_bind_int64(st, 1, ctx->session_id);
        sqlite3_bind_int(st, 2, block_sec);

        size_t cap = 1024, len = 0;
        char *out = malloc(cap);
        if (!out) { sqlite3_finalize(st); return strdup("error: OOM"); }
        len += (size_t)snprintf(out, cap, "approvals for session %lld:\n",
                                (long long)ctx->session_id);
        int rows = 0;
        while (sqlite3_step(st) == SQLITE_ROW) {
            rows++;
            char line[256];
            int n = snprintf(line, sizeof(line),
                "  #%lld %s — %s%s%s (block_until=%lld, expires_at=%lld)\n",
                (long long)sqlite3_column_int64(st, 0),
                (const char *)sqlite3_column_text(st, 1),
                (const char *)sqlite3_column_text(st, 2),
                sqlite3_column_text(st, 3) && sqlite3_column_text(st, 3)[0] ? " via " : "",
                (const char *)sqlite3_column_text(st, 3),
                (long long)sqlite3_column_int64(st, 4),
                (long long)sqlite3_column_int64(st, 5));
            if (n < 0) continue;
            if (len + (size_t)n + 1 > cap) {
                cap = (len + (size_t)n + 1) * 2;
                char *tmp = realloc(out, cap);
                if (!tmp) break;
                out = tmp;
            }
            memcpy(out + len, line, (size_t)n);
            len += (size_t)n;
            out[len] = '\0';
        }
        sqlite3_finalize(st);
        if (rows == 0) {
            free(out);
            return strdup("no approvals for this session");
        }
        return out;
    }

    if (strcmp(action, "rerequest") == 0) {
        int aid = targ_int(&ta, "approval_id", -1);
        if (aid < 0) { tool_parse_free(&ta); return strdup("error: missing approval_id"); }

        /* Must be a terminal (decided) approval owned by this session. */
        const char *sql =
            "SELECT tool_name, action, args_json, resolve FROM approvals"
            " WHERE id=?1 AND session_id=?2 AND state != 'pending';";
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(ctx->db, sql, -1, &st, NULL) != SQLITE_OK) {
            tool_parse_free(&ta);
            return strdup("error: db query failed");
        }
        sqlite3_bind_int(st, 1, aid);
        sqlite3_bind_int64(st, 2, ctx->session_id);
        char *tool_name = NULL, *act = NULL, *args = NULL, *resolve = NULL;
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char *c;
            if ((c = sqlite3_column_text(st, 0))) tool_name = strdup((const char *)c);
            if ((c = sqlite3_column_text(st, 1))) act = strdup((const char *)c);
            if ((c = sqlite3_column_text(st, 2))) args = strdup((const char *)c);
            if ((c = sqlite3_column_text(st, 3))) resolve = strdup((const char *)c);
        }
        sqlite3_finalize(st);
        tool_parse_free(&ta);

        if (!act && !tool_name) {
            free(tool_name); free(act); free(args); free(resolve);
            return strdup("error: no decided approval with that id in this session");
        }
        /* Fresh pending row, not tied to any frozen tool_call (not re-parked):
         * a later decision is delivered to the inbox via the post-window path. */
        int64_t new_id = approval_create(ctx->db, ctx->session_id, NULL,
                                         tool_name, act, args, resolve);
        free(tool_name); free(act); free(args); free(resolve);
        if (new_id < 0) return strdup("error: failed to create approval");
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "re-requested as approval #%lld — the decision will arrive in your inbox.",
                 (long long)new_id);
        return strdup(buf);
    }

    tool_parse_free(&ta);
    return strdup("error: unknown action (use 'status' or 'rerequest')");
}

int tool_check_approval_register(ToolRegistry *reg, AgentLaunchCtx *ctx) {
    int rc = tools_register(reg, "check_approval",
                          "Inspect this session's approvals, or re-raise a decided one",
                          CHECK_APPROVAL_PARAMS_JSON, tool_check_approval_handler, ctx);
    if (rc == 0)
        tools_set_recipe(reg, "check_approval", (ToolRecipe){EXEC_INLINE, SBX_NONE, NULL});
    return rc;
}
