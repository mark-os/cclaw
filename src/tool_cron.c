#define _POSIX_C_SOURCE 200809L
#include "tool_cron.h"
#include "cron.h"
#include "tool_parse.h"
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
    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON");

    const char *name = targ_str(&ta, "name");
    const char *expr = targ_str(&ta, "cron_expr");
    const char *task = targ_str(&ta, "task");

    if (!name || !expr || !task) {
        tool_parse_free(&ta);
        return strdup("error: missing required fields (name, cron_expr, task)");
    }

    int64_t id = cron_add(ctx->db, ctx->agent_name, name,
                          expr, ctx->session_id, task);

    if (id < 0) {
        tool_parse_free(&ta);
        return strdup("error: invalid cron expression or DB error");
    }

    char *result = malloc(128);
    if (!result) { tool_parse_free(&ta); return strdup("error: OOM"); }
    snprintf(result, 128, "created cron job id=%lld name=\"%s\"",
             (long long)id, name);
    tool_parse_free(&ta);
    return result;
}

char *tool_cron_list_handler(const char *arguments, void *user_data) {
    (void)arguments;
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;

    int count = 0;
    CronJob *jobs = cron_list(ctx->db, ctx->agent_name, &count);
    if (count == 0) return strdup("no cron jobs");

    /* Build JSON array manually */
    size_t cap = 256 * (size_t)count + 2;
    char *buf = malloc(cap);
    if (!buf) { cron_list_free(jobs, count); return strdup("error: OOM"); }
    size_t pos = 0;
    buf[pos++] = '[';
    for (int i = 0; i < count; i++) {
        char esc_name[128], esc_expr[128], esc_task[256];
        json_escape_into(esc_name, sizeof(esc_name), jobs[i].name ? jobs[i].name : "");
        json_escape_into(esc_expr, sizeof(esc_expr), jobs[i].cron_expr ? jobs[i].cron_expr : "");
        json_escape_into(esc_task, sizeof(esc_task), jobs[i].task ? jobs[i].task : "");
        int n = snprintf(buf + pos, cap - pos,
            "%s{\"id\":%lld,\"name\":\"%s\",\"cron_expr\":\"%s\","
            "\"task\":\"%s\",\"enabled\":%s,\"next_run_at\":%lld}",
            i ? "," : "", (long long)jobs[i].id, esc_name, esc_expr,
            esc_task, jobs[i].enabled ? "true" : "false",
            (long long)jobs[i].next_run_at);
        if (n < 0 || (size_t)n >= cap - pos) break;
        pos += (size_t)n;
    }
    buf[pos++] = ']';
    buf[pos] = '\0';
    cron_list_free(jobs, count);
    return buf;
}

char *tool_cron_remove_handler(const char *arguments, void *user_data) {
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;
    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0) return strdup("error: invalid JSON");

    int id = targ_int(&ta, "id", -1);
    tool_parse_free(&ta);

    if (id < 0)
        return strdup("error: missing required field 'id'");

    if (cron_remove(ctx->db, (int64_t)id) != 0)
        return strdup("error: job not found or DB error");

    char *result = malloc(64);
    if (!result) return strdup("error: OOM");
    snprintf(result, 64, "removed cron job id=%d", id);
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
