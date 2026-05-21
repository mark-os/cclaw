#define _POSIX_C_SOURCE 200809L
#include "tool_cron.h"
#include "cron.h"
#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *CRON_SET_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Job name\"},"
    "\"cron_expr\":{\"type\":\"string\",\"description\":\"5-field cron expression (M H D Mo DoW)\"},"
    "\"task\":{\"type\":\"string\",\"description\":\"Message to inject when triggered\"}"
    "},\"required\":[\"name\",\"cron_expr\",\"task\"]}";

static const char *CRON_LIST_PARAMS =
    "{\"type\":\"object\",\"properties\":{}}";

static const char *CRON_REMOVE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"integer\",\"description\":\"Job ID to remove\"}"
    "},\"required\":[\"id\"]}";

char *tool_cron_set_handler(const char *arguments, void *user_data) {
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;
    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON");

    cJSON *name = cJSON_GetObjectItemCaseSensitive(json, "name");
    cJSON *expr = cJSON_GetObjectItemCaseSensitive(json, "cron_expr");
    cJSON *task = cJSON_GetObjectItemCaseSensitive(json, "task");

    if (!cJSON_IsString(name) || !cJSON_IsString(expr) || !cJSON_IsString(task)) {
        cJSON_Delete(json);
        return strdup("error: missing required fields (name, cron_expr, task)");
    }

    int64_t id = cron_add(ctx->db, name->valuestring, expr->valuestring,
                          ctx->session_id, task->valuestring);

    char *result = malloc(128);
    if (!result) { cJSON_Delete(json); return strdup("error: OOM"); }

    if (id < 0) {
        cJSON_Delete(json);
        free(result);
        return strdup("error: invalid cron expression or DB error");
    }

    snprintf(result, 128, "created cron job id=%lld name=\"%s\"",
             (long long)id, name->valuestring);
    cJSON_Delete(json);
    return result;
}

char *tool_cron_list_handler(const char *arguments, void *user_data) {
    (void)arguments;
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;

    int count = 0;
    CronJob *jobs = cron_list(ctx->db, ctx->session_id, &count);
    if (count == 0) return strdup("no cron jobs");

    /* Build JSON array output */
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < count; i++) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddNumberToObject(obj, "id", (double)jobs[i].id);
        cJSON_AddStringToObject(obj, "name", jobs[i].name ? jobs[i].name : "");
        cJSON_AddStringToObject(obj, "cron_expr", jobs[i].cron_expr ? jobs[i].cron_expr : "");
        cJSON_AddStringToObject(obj, "task", jobs[i].task ? jobs[i].task : "");
        cJSON_AddBoolToObject(obj, "enabled", jobs[i].enabled);
        cJSON_AddNumberToObject(obj, "next_run_at", (double)jobs[i].next_run_at);
        cJSON_AddItemToArray(arr, obj);
    }
    cron_list_free(jobs, count);

    char *result = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return result ? result : strdup("error: JSON serialization failed");
}

char *tool_cron_remove_handler(const char *arguments, void *user_data) {
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;
    cJSON *json = cJSON_Parse(arguments);
    if (!json) return strdup("error: invalid JSON");

    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(json, "id");
    if (!cJSON_IsNumber(id_item)) {
        cJSON_Delete(json);
        return strdup("error: missing required field 'id'");
    }

    int64_t id = (int64_t)id_item->valuedouble;
    cJSON_Delete(json);

    if (cron_remove(ctx->db, id) != 0)
        return strdup("error: job not found or DB error");

    char *result = malloc(64);
    if (!result) return strdup("error: OOM");
    snprintf(result, 64, "removed cron job id=%lld", (long long)id);
    return result;
}

int tool_cron_register(ToolRegistry *reg, ToolCronCtx *ctx) {
    if (tools_register(reg, "cron_set", "Create a scheduled cron job",
                       CRON_SET_PARAMS, tool_cron_set_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "cron_list", "List cron jobs for this session",
                       CRON_LIST_PARAMS, tool_cron_list_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "cron_remove", "Remove a cron job by ID",
                       CRON_REMOVE_PARAMS, tool_cron_remove_handler, ctx) != 0)
        return -1;
    return 0;
}
