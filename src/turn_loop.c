#define _POSIX_C_SOURCE 200809L
#include "turn_loop.h"
#include "agent_config.h"
#include "agent_exit.h"
#include "context.h"
#include "db.h"
#include "db_response.h"
#include "llm_child.h"
#include "shutdown.h"
#include "tools.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define TOOL_MAX_OUTPUT (256 * 1024)
#define DEFAULT_MAX_ITERATIONS 25

/* Serialize tool schemas to JSON for CCLAW_TOOLS_JSON env var */
static char *schemas_to_json(const ToolSchema *tools, size_t count) {
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

/* Fork LLM child. Returns LLM_EXIT_STOP, LLM_EXIT_TOOLCALL, or LLM_EXIT_ERROR. */
static int fork_llm(int64_t session_id, const Config *cfg, int is_first_iter) {
    if (is_first_iter && cfg->auto_recall)
        setenv("CCLAW_RECALL", "1", 1);
    else
        setenv("CCLAW_RECALL", "0", 1);

    pid_t pid = fork();
    if (pid < 0) return LLM_EXIT_ERROR;
    if (pid == 0) {
        setpgid(0, 0);
        _exit(llm_child_main(session_id));
    }
    int status;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : LLM_EXIT_ERROR;
}

/* Tools that must run in the parent process (need DB write or user interaction) */
static int tool_needs_parent(const char *name) {
    return strcmp(name, "request_config") == 0 ||
           strcmp(name, "memory_create") == 0 ||
           strcmp(name, "memory_append") == 0 ||
           strcmp(name, "memory_replace") == 0 ||
           strcmp(name, "db_query") == 0 ||
           strcmp(name, "js_eval") == 0 ||
           strcmp(name, "js_define_tool") == 0;
}

/* Fork a tool in a child process, capture stdout as result.
 * Returns heap-allocated result string. */
static char *tool_fork_exec(const char *name, const char *args, ToolRegistry *reg) {
    ToolEntry *e = tools_lookup(reg, name);
    if (!e) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
        return err;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) return strdup("error: pipe() failed");

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return strdup("error: fork() failed"); }

    if (pid == 0) {
        /* Child: run tool, write result to stdout */
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);

        char *result = e->handler(args, e->user_data);
        if (result) {
            size_t len = strlen(result);
            size_t written = 0;
            while (written < len) {
                ssize_t n = write(STDOUT_FILENO, result + written, len - written);
                if (n <= 0) break;
                written += (size_t)n;
            }
            free(result);
        }
        _exit(0);
    }

    /* Parent: read result from pipe */
    close(pipefd[1]);
    char *output = malloc(TOOL_MAX_OUTPUT + 1);
    if (!output) { close(pipefd[0]); kill(pid, SIGKILL); waitpid(pid, NULL, 0); return strdup("error: OOM"); }

    size_t out_len = 0;
    while (out_len < TOOL_MAX_OUTPUT) {
        ssize_t n = read(pipefd[0], output + out_len, TOOL_MAX_OUTPUT - out_len);
        if (n <= 0) break;
        out_len += (size_t)n;
    }
    close(pipefd[0]);
    output[out_len] = '\0';

    waitpid(pid, NULL, 0);
    return output;
}

/* Execute a single tool call. Returns heap-allocated result. */
static char *dispatch_tool(const char *name, const char *args,
                           ToolRegistry *reg, int mode,
                           sqlite3 *db, const Config *cfg,
                           int *parked) {
    (void)cfg;
    *parked = 0;

    /* request_config needs special handling per mode */
    if (strcmp(name, "request_config") == 0) {
        if (mode == TURN_MODE_CLI) {
            /* Prompt user for approval */
            printf("\033[33m⚠ config request:\033[0m %s\n", args);
            const char *yolo = getenv("CCLAW_YOLO");
            if (yolo && yolo[0] == '1') {
                /* Auto-approve in yolo mode — apply config */
            } else {
                printf("allow? [y/n]: ");
                fflush(stdout);
                char buf[16];
                if (!fgets(buf, sizeof(buf), stdin) || (buf[0] != 'y' && buf[0] != 'Y'))
                    return strdup("denied by user");
            }
            /* Apply config: parse action + tool/host, call daemon_apply_config */
            cJSON *j = cJSON_Parse(args);
            if (!j) return strdup("error: invalid args JSON");
            cJSON *action = cJSON_GetObjectItem(j, "action");
            char *result = NULL;
            if (action && strcmp(action->valuestring, "grant_tool") == 0) {
                cJSON *tool = cJSON_GetObjectItem(j, "tool");
                if (tool && tool->valuestring[0]) {
                    /* Get agent_name from env */
                    const char *aname = getenv("CCLAW_AGENT_NAME");
                    if (aname && agent_config_add_tool(db, aname, tool->valuestring) == 0) {
                        char buf2[128];
                        snprintf(buf2, sizeof(buf2), "granted tool: %s", tool->valuestring);
                        result = strdup(buf2);
                    } else {
                        result = strdup("error: failed to grant tool");
                    }
                }
            } else if (action && strcmp(action->valuestring, "grant_host") == 0) {
                cJSON *host = cJSON_GetObjectItem(j, "host");
                if (host && host->valuestring[0]) {
                    const char *aname = getenv("CCLAW_AGENT_NAME");
                    if (aname && agent_config_add_host(db, aname, host->valuestring) == 0) {
                        char buf2[128];
                        snprintf(buf2, sizeof(buf2), "granted host: %s", host->valuestring);
                        result = strdup(buf2);
                    } else {
                        result = strdup("error: failed to grant host");
                    }
                }
            }
            cJSON_Delete(j);
            if (!result) result = strdup("error: unknown action");
            printf("\033[36mconfig: %s\033[0m\n", result);
            return result;
        } else {
            /* Daemon mode: park the session */
            *parked = 1;
            return NULL;
        }
    }

    /* Parent-handled tools: run in-process */
    if (tool_needs_parent(name)) {
        ToolEntry *e = tools_lookup(reg, name);
        if (!e) {
            char *err = malloc(128);
            if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
            return err;
        }
        return e->handler(args, e->user_data);
    }

    /* Fork-handled tools */
    return tool_fork_exec(name, args, reg);
}

int turn_loop_run(sqlite3 *db, int64_t session_id, const Config *cfg,
                  AgentSetup *setup, int mode) {
    /* Drain inbox into session entries */
    inbox_consume_into_entries(db, session_id, 100);

    /* Build tool schemas (filtered by whitelist from env) */
    const char *tools_env = getenv("CCLAW_TOOLS");
    const char **whitelist = NULL;
    size_t wl_count = 0;
    char *tools_dup = NULL;

    if (tools_env && tools_env[0]) {
        tools_dup = strdup(tools_env);
        size_t cap = 1;
        for (const char *p = tools_env; *p; p++) if (*p == ',') cap++;
        whitelist = malloc(cap * sizeof(char *));
        if (whitelist && tools_dup) {
            char *tok = strtok(tools_dup, ",");
            while (tok) {
                while (*tok == ' ') tok++;
                if (*tok) whitelist[wl_count++] = tok;
                tok = strtok(NULL, ",");
            }
        }
    }

    ToolSchema schemas[TOOLS_MAX];
    size_t tool_count = tools_schemas_filtered(&setup->reg, whitelist, wl_count,
                                               schemas, TOOLS_MAX);

    /* Serialize tool schemas to env for LLM child */
    char *tools_json = schemas_to_json(schemas, tool_count);
    if (tools_json) { setenv("CCLAW_TOOLS_JSON", tools_json, 1); free(tools_json); }

    int max_iter = cfg->max_iterations > 0 ? cfg->max_iterations : DEFAULT_MAX_ITERATIONS;
    int rc = TURN_EXIT_DONE;

    for (int iter = 0; iter < max_iter; iter++) {
        if (shutdown_requested()) { rc = TURN_EXIT_ERROR; break; }

        /* Fork LLM child */
        int llm_rc = fork_llm(session_id, cfg, iter == 0);

        if (llm_rc == LLM_EXIT_ERROR) { rc = TURN_EXIT_ERROR; break; }
        if (llm_rc == LLM_EXIT_STOP) { rc = TURN_EXIT_DONE; break; }

        /* LLM returned tool_calls — read them from DB */
        int tc_count = 0;
        PendingToolCall *calls = db_tool_call_get_pending(db, session_id, &tc_count);
        if (!calls || tc_count == 0) { rc = TURN_EXIT_ERROR; break; }

        int64_t turn_id = calls[0].turn_id;
        if (turn_id == 0) turn_id = db_next_turn_id(db, session_id);

        for (int i = 0; i < tc_count; i++) {
            if (shutdown_requested()) {
                /* Write error results for remaining */
                for (int j = i; j < tc_count; j++) {
                    ToolResult tr = {.tool_call_id = calls[j].call_id,
                                     .content = "error: agent terminated"};
                    Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                                   .tool_name = calls[j].name, .is_error = 1};
                    entry_append_with_turn(db, session_id, &msg, turn_id);
                    db_tool_call_set_status(db, session_id, calls[j].call_id, "done", NULL);
                }
                rc = TURN_EXIT_ERROR;
                goto done;
            }

            /* CLI progress */
            if (mode == TURN_MODE_CLI) {
                fprintf(stdout, "\n\033[2m[tool: %s]\033[0m ", calls[i].name);
                fflush(stdout);
            }

            /* Dispatch */
            int parked = 0;
            char *result = dispatch_tool(calls[i].name, calls[i].arguments,
                                         &setup->reg, mode, db, cfg, &parked);

            if (parked) {
                /* Daemon mode: mark awaiting_approval, park session */
                db_tool_call_set_status(db, session_id, calls[i].call_id,
                                        "awaiting_approval", NULL);
                session_set_state(db, session_id, "waiting");
                rc = TURN_EXIT_PARKED;
                db_tool_call_free_pending(calls, tc_count);
                goto done;
            }

            if (!result) result = strdup("error: tool returned null");

            /* CLI progress: show result snippet */
            if (mode == TURN_MODE_CLI) {
                size_t rlen = strlen(result);
                if (rlen <= 80)
                    fprintf(stdout, "\033[2m→ %s\033[0m\n", result);
                else
                    fprintf(stdout, "\033[2m→ %.77s...\033[0m\n", result);
                fflush(stdout);
            }

            /* Truncate large results */
            char *stored = truncate_and_spill(result, session_id, calls[i].call_id);

            /* Write tool_result entry */
            ToolResult tr = {.tool_call_id = calls[i].call_id,
                             .content = stored ? stored : result};
            int is_err = (result[0] == 'e' && strncmp(result, "error:", 6) == 0);
            Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                           .tool_name = calls[i].name, .is_error = is_err};
            int64_t result_eid = entry_append_with_turn(db, session_id, &msg, turn_id);

            /* Mark tool_call done */
            db_tool_call_complete_with_result(db, calls[i].entry_id,
                                             calls[i].call_id, result_eid);

            free(stored);
            free(result);
        }

        db_tool_call_free_pending(calls, tc_count);
    }

done:
    unsetenv("CCLAW_TOOLS_JSON");
    free(whitelist);
    free(tools_dup);
    return rc;
}
