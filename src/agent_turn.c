#define _POSIX_C_SOURCE 200809L
#include "agent_turn.h"
#include "agent_config.h"
#include "agent_exit.h"
#include "agent_setup.h"
#include "config.h"
#include "context.h"
#include "daemon.h"
#include "db.h"
#include "db_response.h"
#include "hook_dispatch.h"
#include "llm_child.h"
#include "shutdown.h"
#include "tools.h"
#include "cJSON.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

/* V119/V124: default tool whitelist from agent_config.h */
static const char *DEFAULT_TOOLS[] = { AGENT_DEFAULT_TOOLS };
static const size_t DEFAULT_TOOLS_COUNT = AGENT_DEFAULT_TOOLS_COUNT;

/* T300: Serialize tool schemas to JSON string for CCLAW_TOOLS_JSON env var.
 * Returns heap-allocated JSON array string. Caller frees. */
static char *tools_to_json(const ToolSchema *tools, size_t count) {
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        if (tools[i].name) cJSON_AddStringToObject(item, "name", tools[i].name);
        if (tools[i].description) cJSON_AddStringToObject(item, "description", tools[i].description);
        if (tools[i].parameters_json) {
            cJSON *p = cJSON_Parse(tools[i].parameters_json);
            if (p) cJSON_AddItemToObject(item, "parameters", p);
        }
        cJSON_AddItemToArray(arr, item);
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

/* T300: Fork cclaw-llm child, pipe stderr for logging.
 * Returns LLM_EXIT_STOP (0), LLM_EXIT_TOOLCALL (10), or LLM_EXIT_ERROR (1). */
static int fork_llm_child(int64_t session_id, const Config *cfg, int is_first_iter) {
    /* Set recall flag for first iteration only */
    if (is_first_iter && cfg->auto_recall)
        setenv("CCLAW_RECALL", "1", 1);
    else
        setenv("CCLAW_RECALL", "0", 1);

    pid_t pid = fork();
    if (pid < 0) return LLM_EXIT_ERROR;
    if (pid == 0) {
        /* Child: run LLM call and exit */
        setpgid(0, 0);
        _exit(llm_child_main(session_id));
    }

    /* Parent: wait for child */
    int status;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    return LLM_EXIT_ERROR;
}

/* T300: Read pending tool_calls from DB for the last assistant entry.
 * Returns heap-allocated ToolCall array. Caller frees each field + array.
 * Sets *out_entry_id to the assistant entry containing these tool_calls. */
static ToolCall *read_pending_tool_calls(sqlite3 *db, int64_t session_id,
                                         int *out_count, int64_t *out_turn_id,
                                         int64_t *out_entry_id) {
    *out_count = 0;
    *out_turn_id = 0;
    *out_entry_id = 0;

    /* Find pending tool_calls for this session */
    const char *sql =
        "SELECT tc.call_id, tc.name, tc.arguments, e.turn_id, tc.entry_id"
        " FROM tool_calls tc"
        " JOIN entries e ON e.id = tc.entry_id"
        " WHERE tc.session_id=? AND tc.status='pending'"
        " ORDER BY tc.rowid;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);

    int cap = 8, count = 0;
    ToolCall *calls = malloc((size_t)cap * sizeof(ToolCall));
    if (!calls) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            calls = realloc(calls, (size_t)cap * sizeof(ToolCall));
            if (!calls) { sqlite3_finalize(stmt); return NULL; }
        }
        const char *id = (const char *)sqlite3_column_text(stmt, 0);
        const char *name = (const char *)sqlite3_column_text(stmt, 1);
        const char *args = (const char *)sqlite3_column_text(stmt, 2);
        calls[count].id = id ? strdup(id) : strdup("");
        calls[count].name = name ? strdup(name) : strdup("");
        calls[count].arguments = args ? strdup(args) : strdup("{}");
        *out_turn_id = sqlite3_column_int64(stmt, 3);
        *out_entry_id = sqlite3_column_int64(stmt, 4);
        count++;
    }
    sqlite3_finalize(stmt);
    *out_count = count;
    return calls;
}

/* T300: Parent turn loop — fork cclaw-llm → wait → dispatch tools → loop.
 * Returns AGENT_EXIT_* codes. */
static int parent_turn_loop(sqlite3 *db, int64_t session_id, const Config *cfg,
                            ToolRegistry *reg, const ToolSchema *schemas,
                            size_t tool_count, ExtensionCtx *ext_ctx) {
    /* Serialize tool schemas to env for llm child */
    char *tools_json = tools_to_json(schemas, tool_count);
    if (tools_json) {
        setenv("CCLAW_TOOLS_JSON", tools_json, 1);
        free(tools_json);
    }

    /* T260/V111: turnStart hook */
    hook_dispatch_turn_start(ext_ctx);

    int max_iter = cfg->max_iterations > 0 ? cfg->max_iterations : AGENT_DEFAULT_MAX_ITERATIONS;

    for (int iter = 0; iter < max_iter; iter++) {
        if (shutdown_requested()) {
            int64_t tid = db_next_turn_id(db, session_id);
            Message abort_msg = {.role = ROLE_ASSISTANT,
                                 .content = strdup("error: agent terminated by shutdown signal"),
                                 .stop_reason = STOP_REASON_ABORTED,
                                 .model = cfg->provider.model};
            entry_append_with_turn(db, session_id, &abort_msg, tid);
            free(abort_msg.content);
            return AGENT_EXIT_ERROR;
        }

        /* Fork LLM child */
        int llm_rc = fork_llm_child(session_id, cfg, iter == 0);

        if (llm_rc == LLM_EXIT_ERROR) {
            hook_dispatch_turn_end(ext_ctx);
            return AGENT_EXIT_ERROR;
        }

        if (llm_rc == LLM_EXIT_STOP) {
            hook_dispatch_turn_end(ext_ctx);
            return AGENT_EXIT_DONE;
        }

        /* LLM_EXIT_TOOLCALL: read tool_calls from DB, dispatch */
        int tc_count = 0;
        int64_t turn_id = 0;
        int64_t entry_id = 0;
        ToolCall *calls = read_pending_tool_calls(db, session_id, &tc_count, &turn_id, &entry_id);
        if (!calls || tc_count == 0) {
            free(calls);
            hook_dispatch_turn_end(ext_ctx);
            return AGENT_EXIT_ERROR;
        }

        /* Use turn_id from DB; if 0, generate new one */
        if (turn_id == 0) turn_id = db_next_turn_id(db, session_id);

        for (int i = 0; i < tc_count; i++) {
            if (shutdown_requested()) {
                /* Write error results for remaining calls */
                for (int j = i; j < tc_count; j++) {
                    ToolResult tr = {.tool_call_id = calls[j].id,
                                     .content = "error: agent terminated by shutdown signal"};
                    Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                        .tool_name = calls[j].name, .is_error = 1};
                    entry_append_with_turn(db, session_id, &tool_msg, turn_id);
                    db_tool_call_complete(db, entry_id, calls[j].id);
                }
                goto cleanup_calls;
            }

            /* V113: beforeToolCall hook */
            char *blocked = hook_dispatch_before_tool_call(ext_ctx, calls[i].name, calls[i].arguments);
            char *result;
            if (blocked) {
                result = blocked;
            } else {
                /* CLI progress */
                fprintf(stdout, "\n\033[2m[tool: %s]\033[0m ", calls[i].name);
                fflush(stdout);

                ToolEntry *e = tools_lookup(reg, calls[i].name);
                if (e)
                    result = e->handler(calls[i].arguments, e->user_data);
                else {
                    result = malloc(128);
                    if (result) snprintf(result, 128, "error: unknown tool '%s'", calls[i].name);
                }
                if (!result) result = strdup("error: tool returned null");

                /* V114: afterToolCall hook */
                char *replaced = hook_dispatch_after_tool_call(ext_ctx, calls[i].name,
                                                              calls[i].arguments, result);
                if (replaced) { free(result); result = replaced; }
            }

            /* CLI progress: show result snippet */
            {
                size_t rlen = result ? strlen(result) : 0;
                if (rlen <= 80)
                    fprintf(stdout, "\033[2m→ %s\033[0m\n", result ? result : "(empty)");
                else
                    fprintf(stdout, "\033[2m→ %.77s...\033[0m\n", result);
                fflush(stdout);
            }

            /* V72/V13: detect exit-code sentinels */
            int sentinel_exit = -1;
            if (strncmp(result, SENTINEL_SPAWN, strlen(SENTINEL_SPAWN)) == 0)
                sentinel_exit = AGENT_EXIT_SPAWN;
            else if (strncmp(result, SENTINEL_APPROVAL, strlen(SENTINEL_APPROVAL)) == 0)
                sentinel_exit = AGENT_EXIT_APPROVAL;
            else if (strncmp(result, SENTINEL_CONFIG, strlen(SENTINEL_CONFIG)) == 0)
                sentinel_exit = AGENT_EXIT_CONFIG;

            if (sentinel_exit >= 0) {
                ToolResult tr = {.tool_call_id = calls[i].id, .content = "PENDING"};
                Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                    .tool_name = calls[i].name};
                entry_append_with_turn(db, session_id, &tool_msg, turn_id);
                free(result);
                for (int j = 0; j < tc_count; j++) {
                    free(calls[j].id); free(calls[j].name); free(calls[j].arguments);
                }
                free(calls);
                unsetenv("CCLAW_TOOLS_JSON");
                return sentinel_exit;
            }

            /* V40: truncate large results */
            char *stored = truncate_and_spill(result, session_id, calls[i].id);
            ToolResult tr = {.tool_call_id = calls[i].id,
                             .content = stored ? stored : result};
            int err = (result[0] == 'e' && strncmp(result, "error:", 6) == 0);
            Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                .tool_name = calls[i].name, .is_error = err};
            entry_append_with_turn(db, session_id, &tool_msg, turn_id);

            /* Mark tool_call as complete */
            db_tool_call_complete(db, entry_id, calls[i].id);

            free(stored);
            free(result);
        }

        for (int j = 0; j < tc_count; j++) {
            free(calls[j].id); free(calls[j].name); free(calls[j].arguments);
        }
        free(calls);
        continue;

cleanup_calls:
        for (int j = 0; j < tc_count; j++) {
            free(calls[j].id); free(calls[j].name); free(calls[j].arguments);
        }
        free(calls);
        hook_dispatch_turn_end(ext_ctx);
        unsetenv("CCLAW_TOOLS_JSON");
        return AGENT_EXIT_ERROR;
    }

    /* Max iterations reached */
    hook_dispatch_turn_end(ext_ctx);
    unsetenv("CCLAW_TOOLS_JSON");
    return AGENT_EXIT_ERROR;
}

int agent_turn_run(int64_t session_id) {
    /* V34: die if parent dies */
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    /* V23: resource limits */
    struct rlimit rl;
#ifndef __has_feature
#define __has_feature(x) 0
#endif
#if !defined(__ANDROID__) && !defined(__SANITIZE_ADDRESS__) && !__has_feature(address_sanitizer)
    rl.rlim_cur = 256 * 1024 * 1024;
    rl.rlim_max = 256 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rl);
#endif
    rl.rlim_cur = 300;
    rl.rlim_max = 300;
    setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = 64;
    rl.rlim_max = 64;
    setrlimit(RLIMIT_NOFILE, &rl);

    shutdown_init();

    /* V74,T198: Config purely from env vars */
    Config *cfg = config_load_from_env();
    if (!cfg) return 1;

    /* V73,T198: Open agent's own DB only */
    const char *agent_db_path = getenv("CCLAW_AGENT_DB");
    if (!agent_db_path || !agent_db_path[0])
        agent_db_path = cfg->db_path;

    sqlite3 *db = db_open(agent_db_path);
    if (!db) { config_free(cfg); return 1; }

    /* V57: mmap + reduced cache for agent processes */
    db_set_agent_pragmas(db);

    /* V74: agent_name from env (daemon injects at fork) */
    const char *agent_name_env = getenv("CCLAW_AGENT_NAME");
    char *agent_name = agent_name_env ? strdup(agent_name_env)
                                      : session_get_agent_name(db, session_id);

    /* V74: Parse allowed_hosts from env (comma-separated) — skip in yolo mode */
    char **allowed_hosts = NULL;
    size_t allowed_hosts_count = 0;
    const char *yolo_env = getenv("CCLAW_YOLO");
    int is_yolo = (yolo_env && yolo_env[0] == '1');
    if (!is_yolo) {
        const char *hosts_env = getenv("CCLAW_ALLOWED_HOSTS");
        if (hosts_env && hosts_env[0]) {
            size_t cap = 1;
            for (const char *p = hosts_env; *p; p++) if (*p == ',') cap++;
            allowed_hosts = malloc(cap * sizeof(char *));
            if (allowed_hosts) {
                char *dup = strdup(hosts_env);
                char *tok = strtok(dup, ",");
                while (tok) {
                    while (*tok == ' ') tok++;
                    if (*tok) allowed_hosts[allowed_hosts_count++] = strdup(tok);
                    tok = strtok(NULL, ",");
                }
                free(dup);
            }
        }
    }

    /* V27: Update last_route from newest inbox source before consuming */
    int peek_count = 0;
    InboxItem *items = inbox_peek(db, session_id, 100, &peek_count);
    if (items && peek_count > 0) {
        session_set_last_route(db, session_id, items[peek_count - 1].source);
        inbox_items_free(items, peek_count);
    }

    /* V18: Drain inbox into session entries */
    inbox_consume_into_entries(db, session_id, 100);

    int rc = AGENT_EXIT_DONE;

    /* Check if session is already complete */
    int branch_count = 0;
    Entry *branch = session_get_branch(db, session_id, &branch_count);
    if (branch_count > 0) {
        Entry *last = &branch[branch_count - 1];
        if (last->message.role == ROLE_ASSISTANT &&
            (last->message.stop_reason == STOP_REASON_STOP ||
             last->message.stop_reason == STOP_REASON_ERROR ||
             last->message.stop_reason == STOP_REASON_ABORTED)) {
            entry_branch_free(branch, branch_count);
            goto cleanup;
        }
    }
    if (branch_count == 0) {
        char *prompt = agent_build_system_prompt(db, agent_name, session_id,
                                                "agents", cfg);
        Message sys_msg = {.role = ROLE_SYSTEM, .content = prompt};
        entry_append(db, session_id, &sys_msg);
        free(prompt);
    }
    entry_branch_free(branch, branch_count);

    /* T206: Tool registration (mode from env) */
    const char *mode_env = getenv("CCLAW_MODE");
    int setup_mode = (mode_env && strcmp(mode_env, "cli") == 0)
                     ? AGENT_SETUP_CLI : AGENT_SETUP_DAEMON;
    AgentSetup setup;
    agent_setup_init(&setup, db, session_id, cfg, agent_name,
                     allowed_hosts, allowed_hosts_count, setup_mode);

    size_t tool_count = 0;
    const char *tools_env = getenv("CCLAW_TOOLS");
    const char **tool_whitelist = NULL;
    size_t tool_wl_count = 0;
    char *tools_dup = NULL;
    if (tools_env && tools_env[0]) {
        tools_dup = strdup(tools_env);
        size_t cap = 1;
        for (const char *p = tools_env; *p; p++) if (*p == ',') cap++;
        tool_whitelist = malloc(cap * sizeof(char *));
        if (tool_whitelist && tools_dup) {
            char *tok = strtok(tools_dup, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                if (*tok) tool_whitelist[tool_wl_count++] = tok;
                tok = strtok(NULL, ",");
            }
        }
    } else if (!is_yolo) {
        /* V119/V124: absent CCLAW_TOOLS → use default set (not all tools) */
        tool_whitelist = DEFAULT_TOOLS;
        tool_wl_count = DEFAULT_TOOLS_COUNT;
    }
    const ToolSchema *schemas = tools_schemas_filtered(&setup.reg, tool_whitelist,
                                                       tool_wl_count, &tool_count);

    /* T300: parent turn loop — fork llm child, dispatch tools in-process */
    rc = parent_turn_loop(db, session_id, cfg, &setup.reg, schemas, tool_count,
                          &setup.ext_ctx);

    /* V91: trigger compaction if enabled */
    if (rc == 0 && cfg->compaction && session_needs_compaction(db, session_id, cfg))
        session_try_compact(db, session_id, cfg);

    /* Notify parent inbox if this is a child session */
    const char *sql = "SELECT parent_session_id FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, session_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int64_t parent_sid = sqlite3_column_int64(stmt, 0);
            if (parent_sid > 0) {
                char payload[256];
                snprintf(payload, sizeof(payload),
                         "{\"event\":\"agent_done\",\"session_id\":%lld,\"status\":\"%s\"}",
                         (long long)session_id, rc == 0 ? "done" : "error");
                inbox_insert(db, parent_sid, "agent", payload);
            }
        }
        sqlite3_finalize(stmt);
    }

    agent_setup_destroy(&setup);
    if (tools_dup) free((void *)tool_whitelist);
    free(tools_dup);

cleanup:
    /* T200/V73: Agent sets own state before exit */
    if (rc == AGENT_EXIT_SPAWN || rc == AGENT_EXIT_APPROVAL || rc == AGENT_EXIT_CONFIG)
        session_set_state(db, session_id, "waiting");
    else
        session_set_state(db, session_id, "idle");

    for (size_t i = 0; i < allowed_hosts_count; i++) free(allowed_hosts[i]);
    free(allowed_hosts);
    free(agent_name);
    config_free(cfg);
    db_close(db);

    /* V72/T197: propagate exit codes 0-4 to process exit */
    if (rc >= AGENT_EXIT_DONE && rc <= AGENT_EXIT_CONFIG)
        return rc;
    return AGENT_EXIT_ERROR;
}
