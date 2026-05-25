#define _GNU_SOURCE
#include "tool_shell.h"
#include "http_policy.h"
#include "js_http_fetch.h"
#include <cJSON.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>
#include <arpa/inet.h>

#define SHELL_MAX_OUTPUT (256 * 1024)

static const char *SHELL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 30)\"}"
    "},\"required\":[\"command\"]}";

/* Read all available data from fd into buf. Returns bytes read. */
static size_t read_all(int fd, char *buf, size_t cap) {
    size_t total = 0;
    while (total < cap) {
        ssize_t n = read(fd, buf + total, cap - total);
        if (n <= 0) break;
        total += (size_t)n;
    }
    return total;
}

/* V37: Apply namespace sandbox in child. Returns 0 on success, -1 on failure (fallback). */
static int apply_namespace_sandbox(const char *workspace, int shell_network) {
    int flags = CLONE_NEWUSER | CLONE_NEWNS;
    if (!shell_network) flags |= CLONE_NEWNET;

    if (unshare(flags) != 0) return -1;

    /* Remount / as read-only recursively */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) return 0;
    if (mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY | MS_BIND, NULL) != 0) {
        /* Try alternative: bind-remount */
        mount(NULL, "/", NULL, MS_REC | MS_SLAVE, NULL);
    }

    /* Bind-mount workspace read-write */
    if (workspace) {
        mount(workspace, workspace, NULL, MS_BIND, NULL);
        mount(NULL, workspace, NULL, MS_REMOUNT | MS_BIND, NULL);
    }

    return 0;
}

/* V49/V50: Handle one fetch proxy request on fetch_fd.
 * Reads request, checks policy, performs HTTP, writes response.
 * Returns 0 on success, -1 if fd closed/error (stop proxying). */
static int handle_fetch_request(int fetch_fd, char **allowed_hosts, size_t hosts_count) {
    /* Read 4-byte length header */
    uint32_t req_len_net;
    ssize_t n = read(fetch_fd, &req_len_net, 4);
    if (n <= 0) return -1;
    if (n != 4) return -1;

    uint32_t req_len = ntohl(req_len_net);
    if (req_len == 0 || req_len > 65536) return -1;

    char *req_buf = malloc(req_len + 1);
    if (!req_buf) return -1;

    size_t total = 0;
    while (total < req_len) {
        ssize_t r = read(fetch_fd, req_buf + total, req_len - total);
        if (r <= 0) { free(req_buf); return -1; }
        total += (size_t)r;
    }
    req_buf[req_len] = '\0';

    /* Parse request JSON: {"url","method","headers","body"} */
    cJSON *req_json = cJSON_Parse(req_buf);
    free(req_buf);
    if (!req_json) {
        /* Send error response */
        const char *err = "{\"status\":0,\"headers\":{},\"body\":null,\"error\":\"invalid request JSON\"}";
        uint32_t elen = (uint32_t)strlen(err);
        uint32_t elen_net = htonl(elen);
        write(fetch_fd, &elen_net, 4);
        write(fetch_fd, err, elen);
        return 0;
    }

    const char *url = NULL, *method = NULL, *body = NULL;
    cJSON *u = cJSON_GetObjectItemCaseSensitive(req_json, "url");
    if (cJSON_IsString(u)) url = u->valuestring;
    cJSON *m = cJSON_GetObjectItemCaseSensitive(req_json, "method");
    if (cJSON_IsString(m)) method = m->valuestring;
    cJSON *b = cJSON_GetObjectItemCaseSensitive(req_json, "body");
    if (cJSON_IsString(b)) body = b->valuestring;

    /* Build response JSON */
    char *resp_str = NULL;

    if (!url) {
        resp_str = strdup("{\"status\":0,\"headers\":{},\"body\":null,\"error\":\"missing url\"}");
    } else {
        /* V46: Check policy */
        HttpPolicy policy = {
            .allowed_hosts = allowed_hosts,
            .allowed_count = hosts_count,
            .blocked_hosts = NULL,
            .blocked_count = 0,
            .block_private = 1
        };

        char err[256];
        if (!allowed_hosts || hosts_count == 0) {
            resp_str = strdup("{\"status\":403,\"headers\":{},\"body\":null,\"error\":\"no allowed_hosts configured\"}");
        } else if (http_check_policy(url, &policy, err, sizeof(err)) != 0) {
            /* Policy denied */
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddNumberToObject(resp, "status", 403);
            cJSON_AddItemToObject(resp, "headers", cJSON_CreateObject());
            cJSON_AddNullToObject(resp, "body");
            cJSON_AddStringToObject(resp, "error", err);
            resp_str = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
        } else {
            /* Perform HTTP request via js_http_fetch_exec (reuses existing curl logic) */
            JsHttpResult result = js_http_fetch_exec(url, method, body,
                                                     allowed_hosts, hosts_count);
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddNumberToObject(resp, "status", result.status);
            cJSON_AddItemToObject(resp, "headers", cJSON_CreateObject());
            if (result.body)
                cJSON_AddStringToObject(resp, "body", result.body);
            else
                cJSON_AddNullToObject(resp, "body");
            if (result.error)
                cJSON_AddStringToObject(resp, "error", result.error);
            else
                cJSON_AddNullToObject(resp, "error");
            resp_str = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
            js_http_result_free(&result);
        }
    }

    cJSON_Delete(req_json);

    if (!resp_str)
        resp_str = strdup("{\"status\":0,\"headers\":{},\"body\":null,\"error\":\"internal error\"}");

    /* V50: Write <4B len big-endian><JSON> response */
    uint32_t resp_len = (uint32_t)strlen(resp_str);
    uint32_t resp_len_net = htonl(resp_len);
    int wrc = 0;
    if (write(fetch_fd, &resp_len_net, 4) != 4) wrc = -1;
    if (wrc == 0) {
        size_t written = 0;
        while (written < resp_len) {
            ssize_t w = write(fetch_fd, resp_str + written, resp_len - written);
            if (w <= 0) { wrc = -1; break; }
            written += (size_t)w;
        }
    }
    free(resp_str);
    return wrc;
}

char *tool_shell_handler(const char *arguments, void *user_data) {
    int default_timeout = TOOL_SHELL_DEFAULT_TIMEOUT;
    const char *workspace = NULL;
    int shell_network = 0;
    char **allowed_hosts = NULL;
    size_t allowed_hosts_count = 0;

    if (user_data) {
        ShellConfig *sc = (ShellConfig *)user_data;
        if (sc->timeout > 0) default_timeout = sc->timeout;
        workspace = sc->workspace;
        shell_network = sc->shell_network;
        allowed_hosts = sc->allowed_hosts;
        allowed_hosts_count = sc->allowed_hosts_count;
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

    /* V49: socketpair for mjs fetch proxy */
    int fetch_sv[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fetch_sv) != 0) {
        /* Non-fatal: mjs fetch just won't work */
        fetch_sv[0] = fetch_sv[1] = -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (fetch_sv[0] >= 0) { close(fetch_sv[0]); close(fetch_sv[1]); }
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

        /* V49: set up fetch fd for mjs */
        if (fetch_sv[0] >= 0) {
            close(fetch_sv[0]); /* parent end */
            char fd_str[16];
            snprintf(fd_str, sizeof(fd_str), "%d", fetch_sv[1]);
            setenv("MJS_FETCH_FD", fd_str, 1);
            /* Clear CLOEXEC so fd survives exec */
            int flags = fcntl(fetch_sv[1], F_GETFD);
            if (flags >= 0) fcntl(fetch_sv[1], F_SETFD, flags & ~FD_CLOEXEC);
        }

        /* V37: namespace sandbox — graceful fallback on failure */
        if (apply_namespace_sandbox(workspace, shell_network) != 0) {
            fprintf(stderr, "[shell_exec] warning: namespace sandbox unavailable, running unsandboxed\n");
        }

        /* V47: PATH restriction + env hardening */
        setenv("PATH", "/bin:/usr/bin", 1);
        unsetenv("OPENROUTER_API_KEY");
        unsetenv("GEMINI_API_KEY");
        unsetenv("HOME");
        /* Unset all CCLAW_* env vars */
        extern char **environ;
        for (int i = 0; environ[i]; ) {
            if (strncmp(environ[i], "CCLAW_", 6) == 0) {
                /* unsetenv shifts environ, don't increment */
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
    if (fetch_sv[1] >= 0) close(fetch_sv[1]); /* child end */
    cJSON_Delete(json);

    char *output = malloc(SHELL_MAX_OUTPUT + 1);
    if (!output) {
        close(pipefd[0]);
        if (fetch_sv[0] >= 0) close(fetch_sv[0]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return strdup("error: out of memory");
    }

    /* Poll with timeout — monitor stdout pipe + fetch fd */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += timeout;

    size_t out_len = 0;
    int timed_out = 0;
    int status = 0;
    int stdout_eof = 0;

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
        int maxfd = -1;
        if (!stdout_eof) {
            FD_SET(pipefd[0], &rfds);
            if (pipefd[0] > maxfd) maxfd = pipefd[0];
        }
        if (fetch_sv[0] >= 0) {
            FD_SET(fetch_sv[0], &rfds);
            if (fetch_sv[0] > maxfd) maxfd = fetch_sv[0];
        }

        if (maxfd < 0) break; /* nothing to monitor */

        struct timeval tv;
        long remaining = deadline.tv_sec - now.tv_sec;
        if (remaining > 1) remaining = 1;
        tv.tv_sec = remaining > 0 ? remaining : 0;
        tv.tv_usec = 100000; /* 100ms granularity */

        int sel = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0) {
            /* V49: handle fetch proxy requests */
            if (fetch_sv[0] >= 0 && FD_ISSET(fetch_sv[0], &rfds)) {
                if (handle_fetch_request(fetch_sv[0], allowed_hosts, allowed_hosts_count) < 0) {
                    close(fetch_sv[0]);
                    fetch_sv[0] = -1;
                }
            }
            /* Read stdout */
            if (!stdout_eof && FD_ISSET(pipefd[0], &rfds)) {
                ssize_t n = read(pipefd[0], output + out_len, SHELL_MAX_OUTPUT - out_len);
                if (n <= 0) {
                    stdout_eof = 1;
                } else {
                    out_len += (size_t)n;
                    if (out_len >= SHELL_MAX_OUTPUT) stdout_eof = 1;
                }
            }
        } else if (sel < 0 && errno != EINTR) {
            break;
        }
        /* Check if child exited (non-blocking) */
        int wr = waitpid(pid, &status, WNOHANG);
        if (wr > 0) {
            /* Child done — drain remaining output */
            out_len += read_all(pipefd[0], output + out_len, SHELL_MAX_OUTPUT - out_len);
            close(pipefd[0]);
            if (fetch_sv[0] >= 0) close(fetch_sv[0]);
            goto format_result;
        }
    }

    close(pipefd[0]);
    if (fetch_sv[0] >= 0) close(fetch_sv[0]);

    if (timed_out) {
        /* SIGKILL entire process group */
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

    /* Child may still be running if we broke out of loop for other reasons */
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

int tool_shell_register(ToolRegistry *reg, int default_timeout,
                        const char *workspace, int shell_network,
                        char **allowed_hosts, size_t allowed_hosts_count) {
    ShellConfig *sc = malloc(sizeof(ShellConfig));
    if (!sc) return -1;
    sc->timeout = (default_timeout > 0) ? default_timeout : TOOL_SHELL_DEFAULT_TIMEOUT;
    sc->workspace = workspace;  /* caller owns lifetime */
    sc->shell_network = shell_network;
    sc->allowed_hosts = allowed_hosts;
    sc->allowed_hosts_count = allowed_hosts_count;
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
