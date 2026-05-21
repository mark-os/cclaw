#define _POSIX_C_SOURCE 200809L
#include "tool_subagent.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *SPAWN_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"task\":{\"type\":\"string\",\"description\":\"Task description for the sub-agent\"}"
    "},\"required\":[\"task\"]}";

char *tool_spawn_agent_handler(const char *arguments, void *user_data) {
    SubAgentCtx *ctx = (SubAgentCtx *)user_data;
    if (!ctx || !ctx->db || !ctx->self_path) {
        return strdup("error: spawn_agent not configured");
    }

    cJSON *json = cJSON_Parse(arguments);
    if (!json) {
        return strdup("error: invalid JSON arguments");
    }

    cJSON *task_item = cJSON_GetObjectItemCaseSensitive(json, "task");
    if (!cJSON_IsString(task_item) || !task_item->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: missing or empty 'task' field");
    }
    const char *task = task_item->valuestring;

    /* V3: check depth limit */
    if (ctx->depth >= SUBAGENT_MAX_DEPTH) {
        cJSON_Delete(json);
        return strdup("error: max sub-agent depth reached (limit 2)");
    }

    /* V3: check per-parent limit */
    int parent_count = subagent_count_by_parent(ctx->db, ctx->session_id);
    if (parent_count >= SUBAGENT_MAX_PER_PARENT) {
        cJSON_Delete(json);
        return strdup("error: max concurrent sub-agents per parent reached (limit 3)");
    }

    /* V3: check system-wide limit */
    int total_count = subagent_count_total(ctx->db);
    if (total_count >= SUBAGENT_MAX_TOTAL) {
        cJSON_Delete(json);
        return strdup("error: max system-wide sub-agents reached (limit 10)");
    }

    /* Create a session for the sub-agent */
    char name_buf[128];
    snprintf(name_buf, sizeof(name_buf), "sub-agent:%lld", (long long)ctx->session_id);
    int64_t child_session_id = session_create(ctx->db, name_buf);
    if (child_session_id < 0) {
        cJSON_Delete(json);
        return strdup("error: failed to create sub-agent session");
    }

    /* Build args for fork+exec */
    char session_arg[64];
    snprintf(session_arg, sizeof(session_arg), "--session-id=%lld", (long long)child_session_id);

    char task_arg[4096];
    snprintf(task_arg, sizeof(task_arg), "--task=%s", task);

    char depth_arg[32];
    snprintf(depth_arg, sizeof(depth_arg), "--depth=%d", ctx->depth + 1);

    pid_t pid = fork();
    if (pid < 0) {
        cJSON_Delete(json);
        return strdup("error: fork() failed");
    }

    if (pid == 0) {
        /* Child: exec sub-agent */
        setsid();
        execl(ctx->self_path, ctx->self_path, "--sub-agent",
              session_arg, task_arg, depth_arg, (char *)NULL);
        _exit(127);
    }

    /* Parent: record in DB */
    int64_t agent_id = subagent_create(ctx->db, ctx->session_id, child_session_id,
                                       pid, ctx->depth + 1, task);
    cJSON_Delete(json);

    if (agent_id < 0) {
        return strdup("error: failed to record sub-agent in DB");
    }

    /* V13: return agent_id only — result delivered via check_agent */
    char *result = malloc(128);
    if (!result) return strdup("error: OOM");
    snprintf(result, 128, "spawned sub-agent id=%lld (session=%lld, pid=%d)",
             (long long)agent_id, (long long)child_session_id, (int)pid);
    return result;
}

int tool_spawn_agent_register(ToolRegistry *reg, SubAgentCtx *ctx) {
    return tools_register(reg, "spawn_agent",
                          "Fork a sub-agent process to handle a task asynchronously",
                          SPAWN_PARAMS_JSON, tool_spawn_agent_handler, ctx);
}
