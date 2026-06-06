#define _POSIX_C_SOURCE 200809L
#include "tool_agent.h"
#include "tool_parse.h"
#include "daemon.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static const char *SPAWN_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"task\":{\"type\":\"string\",\"description\":\"Task description for the sub-agent\"},"
    "\"name\":{\"type\":\"string\",\"description\":\"Name of existing agent to launch (omit for anonymous sub-agent)\"},"
    "\"background\":{\"type\":\"boolean\",\"description\":\"If true, run in background (default: false, blocking)\"}"
    "},\"required\":[\"task\"]}";

/* Read last assistant message content from a session's branch */
static char *get_session_result(sqlite3 *db, int64_t session_id) {
    return get_response_text(db, session_id);
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
    int background = targ_bool(&ta, "background", 0);

    /* V3: check depth limit (derive from DB) */
    int depth = session_get_depth(ctx->db, ctx->session_id);
    if (depth >= AGENT_MAX_DEPTH) {
        tool_parse_free(&ta);
        return strdup("error: max agent depth reached (limit 2)");
    }

    /* V3: check per-parent limit */
    int parent_count = session_count_children(ctx->db, ctx->session_id);
    if (parent_count >= AGENT_MAX_PER_PARENT) {
        tool_parse_free(&ta);
        return strdup("error: max concurrent sub-agents per parent reached (limit 3)");
    }

    /* V3: check system-wide limit */
    int total_count = session_count_active_agents(ctx->db);
    if (total_count >= AGENT_MAX_TOTAL) {
        tool_parse_free(&ta);
        return strdup("error: max system-wide sub-agents reached (limit 10)");
    }

    /* In daemon mode, insert into spawn_queue and return */
    if (ctx->daemon_mode) {
        tool_parse_free(&ta);
        return strdup(background ? "spawn queued (background)" : "spawn queued (blocking)");
    }

    /* V39: CLI mode — fork+exec the agent binary */
    int64_t child_session_id = session_create(ctx->db, "agent", NULL,
                                              ctx->session_id, depth + 1);
    if (child_session_id < 0) {
        tool_parse_free(&ta);
        return strdup("error: failed to create agent session");
    }
    inbox_insert(ctx->db, child_session_id, "spawn", task);
    tool_parse_free(&ta);

    pid_t pid = fork();
    if (pid < 0)
        return strdup("error: fork() failed");

    if (pid == 0) {
        setsid();
        char sid_arg[64];
        snprintf(sid_arg, sizeof(sid_arg), "--session-id=%lld", (long long)child_session_id);
        execl(ctx->self_path, ctx->self_path, "--agent", sid_arg, (char *)NULL);
        _exit(127);
    }

    if (background) {
        char *result = malloc(128);
        if (!result) return strdup("error: OOM");
        snprintf(result, 128, "launched agent (session=%lld, pid=%d)",
                 (long long)child_session_id, (int)pid);
        return result;
    }

    /* Blocking: wait for child to finish */
    int status = 0;
    waitpid(pid, &status, 0);

    char *result = get_session_result(ctx->db, child_session_id);
    if (result) return result;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return strdup("agent completed with no output");
    return strdup("error: agent exited abnormally");
}

int tool_launch_agent_register(ToolRegistry *reg, AgentLaunchCtx *ctx) {
    return tools_register(reg, "launch_agent",
                          "Launch an agent process to handle a task asynchronously",
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

    int session_id_val = targ_int(&ta, "session_id", -1);
    if (session_id_val < 0) {
        tool_parse_free(&ta);
        return strdup("error: missing or invalid 'session_id' field");
    }
    int64_t child_sid = (int64_t)session_id_val;
    tool_parse_free(&ta);

    /* Query child session state */
    const char *sql = "SELECT state FROM sessions WHERE id=? AND parent_session_id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return strdup("error: db query failed");
    sqlite3_bind_int64(stmt, 1, child_sid);
    sqlite3_bind_int64(stmt, 2, ctx->session_id);
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return strdup("error: sub-agent session not found (or not a child of this session)");
    }
    const char *state = (const char *)sqlite3_column_text(stmt, 0);
    char *state_copy = strdup(state ? state : "unknown");
    sqlite3_finalize(stmt);

    /* Build response */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "session_id", (double)child_sid);
    cJSON_AddStringToObject(resp, "state", state_copy);

    /* If idle (finished), include result */
    if (strcmp(state_copy, "idle") == 0) {
        char *result = get_session_result(ctx->db, child_sid);
        if (result) {
            cJSON_AddStringToObject(resp, "result", result);
            free(result);
        }
    }

    free(state_copy);
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    return out ? out : strdup("error: OOM");
}

int tool_check_agent_register(ToolRegistry *reg, AgentLaunchCtx *ctx) {
    return tools_register(reg, "check_agent",
                          "Check the status and result of a sub-agent session",
                          CHECK_PARAMS_JSON, tool_check_agent_handler, ctx);
}
