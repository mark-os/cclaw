#define _POSIX_C_SOURCE 200809L
#include "tool_cron.h"
#include "config_registry.h"
#include "cron.h"
#include "tool_args.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *CRON_SET_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"name\":{\"type\":\"string\",\"description\":\"Job name\"},"
    "\"cron_expr\":{\"type\":\"string\",\"description\":\"5-field cron expression (M H D Mo DoW), evaluated in UTC, for a recurring job. Omit if using in_seconds.\"},"
    "\"in_seconds\":{\"type\":\"integer\",\"description\":\"Delay in seconds from now for a one-shot job that fires once and is then removed. Omit if using cron_expr.\"},"
    "\"task\":{\"type\":\"string\",\"description\":\"Message to inject into this session when triggered\"}"
    "},\"required\":[\"name\",\"task\"]}";

static const char *CRON_LIST_PARAMS =
    "{\"type\":\"object\",\"properties\":{}}";

static const char *CRON_REMOVE_PARAMS =
    "{\"type\":\"object\",\"properties\":{"
    "\"id\":{\"type\":\"integer\",\"description\":\"Job ID to remove\"}"
    "},\"required\":[\"id\"]}";

/* Number of jobs a session currently owns (per-session cap check). */
static int session_job_count(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cron_jobs WHERE session_id=?",
                           -1, &st, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(st, 1, session_id);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

char *tool_cron_set_handler(const char *arguments, void *user_data) {
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;

    char *name = tool_args_str(ctx->db, arguments, "name");
    char *expr = tool_args_str(ctx->db, arguments, "cron_expr");
    char *task = tool_args_str(ctx->db, arguments, "task");
    int in_seconds = tool_args_int(ctx->db, arguments, "in_seconds", 0);

    if (!name || !task) {
        free(name); free(expr); free(task);
        return strdup("error: missing required fields (name, task)");
    }

    /* Exactly one schedule. in_seconds!=0 counts as provided (a non-positive
     * value is provided-but-invalid; the model must supply a relative delay,
     * not a raw epoch — the handler computes run_at). */
    int has_expr    = (expr && expr[0]);
    int has_seconds = (in_seconds != 0);
    if (has_expr == has_seconds) {   /* both, or neither */
        free(name); free(expr); free(task);
        return strdup("error: provide exactly one of cron_expr or in_seconds");
    }
    if (has_seconds && in_seconds <= 0) {
        free(name); free(expr); free(task);
        return strdup("error: in_seconds must be positive");
    }

    /* Per-session cap — stays at the tool boundary (manifest/heartbeat rows
     * use session_id=0 and are legitimately operator-shaped). */
    int cap = config_get_int(ctx->db, "cron_max_jobs_per_session");
    if (cap > 0) {
        int n = session_job_count(ctx->db, ctx->session_id);
        if (n >= cap) {
            free(name); free(expr); free(task);
            char *e = malloc(96);
            if (!e) return strdup("error: OOM");
            snprintf(e, 96, "error: session cron job limit reached (max %d)", cap);
            return e;
        }
    }

    int64_t run_at = has_seconds ? (int64_t)time(NULL) + in_seconds : 0;
    int64_t id = cron_add(ctx->db, ctx->agent_name, name,
                          has_expr ? expr : "", run_at, 0, ctx->session_id, task);

    if (id < 0) {
        free(name); free(expr); free(task);
        return strdup("error: invalid schedule "
                      "(bad cron_expr, or below the min-interval floor)");
    }

    char *result = malloc(128);
    if (!result) { free(name); free(expr); free(task); return strdup("error: OOM"); }
    snprintf(result, 128, "created cron job id=%lld name=\"%s\"",
             (long long)id, name);
    free(name); free(expr); free(task);
    return result;
}

char *tool_cron_list_handler(const char *arguments, void *user_data) {
    (void)arguments;
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;

    int count = 0;
    CronJob *jobs = cron_list(ctx->db, ctx->agent_name, &count);
    if (count == 0) return strdup("no cron jobs");

    size_t cap = 128 * (size_t)count + 64;
    char *buf = malloc(cap);
    if (!buf) { cron_list_free(jobs, count); return strdup("error: OOM"); }
    size_t pos = 0;

    /* Header. A blank cron_expr with a future next_run_at reads as a one-shot;
     * kind surfaces heartbeat rows. */
    pos += (size_t)snprintf(buf + pos, cap - pos,
        "id|name|kind|cron_expr|task|enabled|next_run_at\n");

    for (int i = 0; i < count; i++) {
        int n = snprintf(buf + pos, cap - pos, "%lld|%s|%s|%s|%s|%s|%lld\n",
            (long long)jobs[i].id,
            jobs[i].name ? jobs[i].name : "",
            jobs[i].kind ? jobs[i].kind : "task",
            jobs[i].cron_expr ? jobs[i].cron_expr : "",
            jobs[i].task ? jobs[i].task : "",
            jobs[i].enabled ? "true" : "false",
            (long long)jobs[i].next_run_at);
        if (n > 0) pos += (size_t)n;
    }
    cron_list_free(jobs, count);
    return buf;
}

char *tool_cron_remove_handler(const char *arguments, void *user_data) {
    ToolCronCtx *ctx = (ToolCronCtx *)user_data;

    int id = tool_args_int(ctx->db, arguments, "id", -1);

    if (id < 0)
        return strdup("error: missing required field 'id'");

    if (cron_remove(ctx->db, (int64_t)id, ctx->agent_name) != 0)
        return strdup("error: job not found or DB error");

    char *result = malloc(64);
    if (!result) return strdup("error: OOM");
    snprintf(result, 64, "removed cron job id=%d", id);
    return result;
}

/* EXEC_THREAD shims: rebuild ToolCronCtx around the thread's own db. cron_set
 * needs session_id (it stamps the job's owning session). */
static char *cron_set_thread_run(sqlite3 *db, const char *agent_name,
                                 int64_t session_id, const char *args) {
    ToolCronCtx c = {.db = db, .session_id = session_id, .agent_name = agent_name};
    return tool_cron_set_handler(args, &c);
}
static char *cron_list_thread_run(sqlite3 *db, const char *agent_name,
                                  int64_t session_id, const char *args) {
    ToolCronCtx c = {.db = db, .session_id = session_id, .agent_name = agent_name};
    return tool_cron_list_handler(args, &c);
}
static char *cron_remove_thread_run(sqlite3 *db, const char *agent_name,
                                    int64_t session_id, const char *args) {
    ToolCronCtx c = {.db = db, .session_id = session_id, .agent_name = agent_name};
    return tool_cron_remove_handler(args, &c);
}

int tool_cron_register(ToolRegistry *reg, ToolCronCtx *ctx) {
    if (tools_register(reg, "cron_set",
                       "Schedule a prompt to yourself: recurring (cron_expr) or "
                       "one-shot (in_seconds). This is how a follow-up promise "
                       "becomes real — schedule it instead of saying 'later'.",
                       CRON_SET_PARAMS, tool_cron_set_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "cron_list", "List cron jobs for this session",
                       CRON_LIST_PARAMS, tool_cron_list_handler, ctx) != 0)
        return -1;
    if (tools_register(reg, "cron_remove", "Remove a cron job by ID",
                       CRON_REMOVE_PARAMS, tool_cron_remove_handler, ctx) != 0)
        return -1;
    tools_set_recipe(reg, "cron_set",    (ToolRecipe){EXEC_THREAD, SBX_NONE, cron_set_thread_run});
    tools_set_recipe(reg, "cron_list",   (ToolRecipe){EXEC_THREAD, SBX_NONE, cron_list_thread_run});
    tools_set_recipe(reg, "cron_remove", (ToolRecipe){EXEC_THREAD, SBX_NONE, cron_remove_thread_run});
    return 0;
}
