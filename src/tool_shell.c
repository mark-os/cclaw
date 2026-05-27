#define _GNU_SOURCE
#include "tool_shell.h"
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define SHELL_MAX_OUTPUT (256 * 1024)

static const char *SHELL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 30)\"}"
    "},\"required\":[\"command\"]}";

char *tool_shell_handler(const char *arguments, void *user_data) {
    int default_timeout = TOOL_SHELL_DEFAULT_TIMEOUT;

    if (user_data) {
        ShellConfig *sc = (ShellConfig *)user_data;
        if (sc->timeout > 0) default_timeout = sc->timeout;
    }

    cJSON *json = cJSON_Parse(arguments);
    if (!json) {
        return strdup("error: invalid JSON arguments");
    }

    cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(json, "command");
    if (!cJSON_IsString(cmd_item) || !cmd_item->valuestring[0]) {
        cJSON_Delete(json);
        return strdup("error: missing or empty 'command' field");
    }
    const char *command = cmd_item->valuestring;

    int timeout = default_timeout;
    cJSON *timeout_item = cJSON_GetObjectItemCaseSensitive(json, "timeout");
    if (cJSON_IsNumber(timeout_item) && timeout_item->valueint > 0) {
        timeout = timeout_item->valueint;
    }

    /* Pipe for child stdout+stderr */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        cJSON_Delete(json);
        return strdup("error: pipe() failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        cJSON_Delete(json);
        return strdup("error: fork() failed");
    }

    if (pid == 0) {
        /* Child: new process group so we can kill entire tree */
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* V47: PATH restriction + env hardening */
        setenv("PATH", "/bin:/usr/bin", 1);
        unsetenv("OPENROUTER_API_KEY");
        unsetenv("GEMINI_API_KEY");
        unsetenv("HOME");
        /* Unset all CCLAW_* env vars */
        extern char **environ;
        for (int i = 0; environ[i]; ) {
            if (strncmp(environ[i], "CCLAW_", 6) == 0) {
                char *eq = strchr(environ[i], '=');
                if (eq) {
                    size_t klen = (size_t)(eq - environ[i]);
                    char key[klen + 1];
                    memcpy(key, environ[i], klen);
                    key[klen] = '\0';
                    unsetenv(key);
                } else {
                    i++;
                }
            } else {
                i++;
            }
        }

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* Parent: ensure child is in its own process group */
    setpgid(pid, pid);
    close(pipefd[1]);
    cJSON_Delete(json);

    char *output = malloc(SHELL_MAX_OUTPUT + 1);
    if (!output) {
        close(pipefd[0]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return strdup("error: out of memory");
    }

    /* Poll with timeout */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout;

    size_t out_len = 0;
    int timed_out = 0;
    int status = 0;

    while (1) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            timed_out = 1;
            break;
        }

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);

        struct timeval tv;
        long remaining = deadline.tv_sec - now.tv_sec;
        if (remaining > 1) remaining = 1;
        tv.tv_sec = remaining > 0 ? remaining : 0;
        tv.tv_usec = 100000;

        int sel = select(pipefd[0] + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0) {
            ssize_t n = read(pipefd[0], output + out_len, SHELL_MAX_OUTPUT - out_len);
            if (n <= 0) break;
            out_len += (size_t)n;
            if (out_len >= SHELL_MAX_OUTPUT) break;
        } else if (sel < 0 && errno != EINTR) {
            break;
        }

        /* Check if child exited (non-blocking) */
        int wr = waitpid(pid, &status, WNOHANG);
        if (wr > 0) {
            /* Drain remaining output */
            while (out_len < SHELL_MAX_OUTPUT) {
                ssize_t n = read(pipefd[0], output + out_len, SHELL_MAX_OUTPUT - out_len);
                if (n <= 0) break;
                out_len += (size_t)n;
            }
            close(pipefd[0]);
            goto format_result;
        }
    }

    close(pipefd[0]);

    if (timed_out) {
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        output[out_len] = '\0';
        size_t needed = out_len + 128;
        char *result = malloc(needed);
        if (!result) { free(output); return strdup("error: timeout + OOM"); }
        snprintf(result, needed, "[timeout after %ds]\n%s", timeout, output);
        free(output);
        return result;
    }

    waitpid(pid, &status, 0);

format_result:
    output[out_len] = '\0';

    int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    size_t needed = out_len + 64;
    char *result = malloc(needed);
    if (!result) { free(output); return strdup("error: OOM"); }
    snprintf(result, needed, "[exit %d]\n%s", exit_code, output);
    free(output);
    return result;
}

int tool_shell_register(ToolRegistry *reg, int default_timeout, const char *workspace) {
    ShellConfig *sc = malloc(sizeof(ShellConfig));
    if (!sc) return -1;
    sc->timeout = (default_timeout > 0) ? default_timeout : TOOL_SHELL_DEFAULT_TIMEOUT;
    sc->workspace = workspace;
    int rc = tools_register(reg, "shell_exec",
                            "Execute a shell command and return stdout+stderr",
                            SHELL_PARAMS_JSON, tool_shell_handler, sc);
    if (rc == 0) {
        ToolEntry *e = tools_lookup(reg, "shell_exec");
        if (e) e->free_fn = free;
    } else {
        free(sc);
    }
    return rc;
}
