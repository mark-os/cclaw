#define _POSIX_C_SOURCE 200809L
#include "tool_subagent.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static const char *SPAWN_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"task\":{\"type\":\"string\",\"description\":\"Task description for the sub-agent\"},"
    "\"background\":{\"type\":\"boolean\",\"description\":\"If true, run in background (default: false, blocking)\"}"
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

    /* V13: blocking by default, background if explicitly requested */
    cJSON *bg_item = cJSON_GetObjectItemCaseSensitive(json, "background");
    int background = cJSON_IsTrue(bg_item);

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

    if (background) {
        /* V13 background: return immediately */
        char *result = malloc(128);
        if (!result) return strdup("error: OOM");
        snprintf(result, 128, "spawned sub-agent id=%lld (session=%lld, pid=%d)",
                 (long long)agent_id, (long long)child_session_id, (int)pid);
        return result;
    }

    /* V13 blocking (default): wait for child, return result */
    int status = 0;
    waitpid(pid, &status, 0);

    /* Read result from DB (child calls subagent_finish before exit) */
    SubAgentInfo *info = subagent_get(ctx->db, agent_id);
    if (!info) {
        return strdup("error: sub-agent finished but no record found");
    }

    char *result;
    if (info->result && info->result[0]) {
        result = strdup(info->result);
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        result = strdup("sub-agent completed with no output");
    } else {
        result = strdup("error: sub-agent exited abnormally");
    }
    subagent_info_free(info);
    return result ? result : strdup("error: OOM");
}

int tool_spawn_agent_register(ToolRegistry *reg, SubAgentCtx *ctx) {
    return tools_register(reg, "spawn_agent",
                          "Fork a sub-agent process to handle a task asynchronously",
                          SPAWN_PARAMS_JSON, tool_spawn_agent_handler, ctx);
}

/* --- check_agent tool (V13) --- */

static const char *CHECK_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"agent_id\":{\"type\":\"integer\",\"description\":\"ID of the sub-agent to check\"}"
    "},\"required\":[\"agent_id\"]}";

char *tool_check_agent_handler(const char *arguments, void *user_data) {
    SubAgentCtx *ctx = (SubAgentCtx *)user_data;
    if (!ctx || !ctx->db) {
        return strdup("error: check_agent not configured");
    }

    cJSON *json = cJSON_Parse(arguments);
    if (!json) {
        return strdup("error: invalid JSON arguments");
    }

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(json, "agent_id");
    if (!cJSON_IsNumber(id_item)) {
        cJSON_Delete(json);
        return strdup("error: missing or invalid 'agent_id' field");
    }
    int64_t agent_id = (int64_t)id_item->valuedouble;
    cJSON_Delete(json);

    SubAgentInfo *info = subagent_get(ctx->db, agent_id);
    if (!info) {
        return strdup("error: sub-agent not found");
    }

    /* Build response */
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddNumberToObject(resp, "agent_id", (double)info->id);
    cJSON_AddStringToObject(resp, "status", info->status);
    cJSON_AddStringToObject(resp, "task", info->task);
    if (info->result) {
        cJSON_AddStringToObject(resp, "result", info->result);
    }

    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    subagent_info_free(info);
    return out ? out : strdup("error: OOM");
}

int tool_check_agent_register(ToolRegistry *reg, SubAgentCtx *ctx) {
    return tools_register(reg, "check_agent",
                          "Read the status and result of a sub-agent (V13: only way to get result)",
                          CHECK_PARAMS_JSON, tool_check_agent_handler, ctx);
}

/* V3/T39: Reap zombie sub-agent processes, mark crashed ones in DB */

int subagent_reap(sqlite3 *db) {
    int count = 0;
    int running = 0;
    SubAgentInfo *list = subagent_list_running(db, &running);
    if (!list) return 0;

    for (int i = 0; i < running; i++) {
        int status = 0;
        pid_t result = waitpid(list[i].pid, &status, WNOHANG);
        if (result > 0) {
            /* Process exited */
            const char *final_status;
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                final_status = "done";
            } else {
                final_status = "crashed";
            }
            /* Only update if subagent_finish hasn't already been called by the child */
            SubAgentInfo *info = subagent_get(db, list[i].id);
            if (info && strcmp(info->status, "running") == 0) {
                subagent_finish(db, list[i].id, final_status,
                               final_status[0] == 'c' ? "process exited abnormally" : NULL);
                count++;
            }
            subagent_info_free(info);
        } else if (result == 0) {
            /* Still running — nothing to do */
        } else {
            /* waitpid error (ECHILD) — process doesn't exist, mark crashed */
            subagent_finish(db, list[i].id, "crashed", "process not found");
            count++;
        }
        free(list[i].status);
        free(list[i].task);
        free(list[i].result);
    }
    free(list);
    return count;
}
