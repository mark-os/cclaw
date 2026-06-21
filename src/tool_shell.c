#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "tool_shell.h"
#include "tool_parse.h"
#include "sandbox.h"
#include "proxy.h"
#include "config.h"

#define SHELL_MAX_OUTPUT (256 * 1024)

static const char *SHELL_PARAMS_JSON =
    "{\"type\":\"object\",\"properties\":{"
    "\"command\":{\"type\":\"string\",\"description\":\"Shell command to execute\"},"
    "\"timeout\":{\"type\":\"integer\",\"description\":\"Timeout in seconds (default 30)\"}"
    "},\"required\":[\"command\"]}";

/* True if the command references $CCLAW_SECRET_<name> as a whole env-var
 * token. Used to scope env injection to referenced secrets only. The trailing
 * char must not extend the identifier, so CCLAW_SECRET_API does not match a
 * reference to $CCLAW_SECRET_API_KEY. */
static int command_uses_secret_env(const char *command, const char *name) {
    char tok[256];
    int tlen = snprintf(tok, sizeof(tok), "CCLAW_SECRET_%s", name);
    if (tlen <= 0 || tlen >= (int)sizeof(tok)) return 0;
    for (const char *p = command; (p = strstr(p, tok)) != NULL; p += tlen) {
        char after = p[tlen];
        if (!((after >= 'A' && after <= 'Z') || (after >= 'a' && after <= 'z') ||
              (after >= '0' && after <= '9') || after == '_'))
            return 1;
    }
    return 0;
}

char *tool_shell_handler(const char *arguments, void *user_data) {
    ShellConfig *sc = (ShellConfig *)user_data;
    int default_timeout = (sc && sc->timeout > 0) ? sc->timeout
                                                  : TOOL_SHELL_DEFAULT_TIMEOUT;

    ToolArgs ta;
    if (tool_parse(arguments, &ta) != 0)
        return strdup("error: invalid JSON arguments");

    const char *command = targ_str(&ta, "command");
    if (!command || !command[0]) {
        tool_parse_free(&ta);
        return strdup("error: missing or empty 'command' field");
    }

    int timeout = targ_int(&ta, "timeout", default_timeout);
    if (timeout <= 0) timeout = default_timeout;

    /* Pipe for child stdout+stderr */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        tool_parse_free(&ta);
        return strdup("error: pipe() failed");
    }

    /* Per-call egress proxy. Bind the UDS now, while this broker is still
     * single-threaded, so the sandbox fork below stays single-threaded (no
     * fork-in-threaded-process hazard); the accept thread starts afterward.
     * Policy + the outbound dial socket thus live in this disposable per-call
     * process, never the long-lived daemon. Skipped when the trust level has no
     * network (net_mode) or no sandbox (host) — both reach the network without
     * the proxy or not at all. */
    ProxyContext proxy;
    const char *psock = NULL;
    int proxy_active = 0;
    char sockdir[PATH_MAX];
    if (sc && sc->sandbox && !sc->net_mode &&
        ((sc->workspace && sc->workspace[0]) || (sc->db_path && sc->db_path[0]))) {
        /* The control-plane socket lives in the agent folder, never the
         * agent-visible workspace (bind-mounted rw + listed by the file tools). */
        agent_dir_resolve(sc->workspace, sc->db_path, sockdir, sizeof(sockdir));
        mkdir(sockdir, 0700);  /* best-effort: parent-of-workspace already exists */
        if (proxy_bind(&proxy, sockdir, sc->allowed_hosts, sc->allowed_host_count) == 0) {
            psock = proxy_sock_path(&proxy);
            proxy_active = 1;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        if (proxy_active) proxy_stop(&proxy);
        tool_parse_free(&ta);
        return strdup("error: fork() failed");
    }

    if (pid == 0) {
        /* Child: new process group so we can kill entire tree */
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        /* V82/V37: namespace sandbox + env scrub + rlimits + proxy preload.
         * sandbox_child_setup fails closed — _exit if the sandbox can't be
         * established (stderr is captured into the tool output). */
        ShellConfig *sc_child = user_data ? (ShellConfig *)user_data : NULL;
        SandboxConfig cfg = {0};
        if (sc_child) {
            cfg.workspace       = sc_child->workspace;
            cfg.cwd_path        = sc_child->cwd_path;
            cfg.db_path         = sc_child->db_path;
            cfg.env_file        = sc_child->env_file;
            cfg.proxy_sock      = psock;
            cfg.sandbox         = sc_child->sandbox;
            cfg.workspace_ro    = sc_child->workspace_ro;
            cfg.mount_cwd       = sc_child->mount_cwd;
            cfg.net_mode        = sc_child->net_mode;
            cfg.env_mode        = sc_child->env_mode;
            cfg.rlimits.nproc   = sc_child->rlimits.nproc;
            cfg.rlimits.as_mb   = sc_child->rlimits.as_mb;
            cfg.rlimits.cpu_sec = sc_child->rlimits.cpu_sec;
            /* Layer 2: build extra_mounts from read/write path grants */
            size_t n_extra = sc_child->read_path_count + sc_child->write_path_count;
            if (n_extra > 0) {
                cfg.extra_mounts = malloc(n_extra * sizeof(*cfg.extra_mounts));
                if (cfg.extra_mounts) {
                    size_t j = 0;
                    for (size_t i = 0; i < sc_child->read_path_count; i++) {
                        cfg.extra_mounts[j].path = sc_child->read_paths[i];
                        cfg.extra_mounts[j].ro = 1;
                        j++;
                    }
                    for (size_t i = 0; i < sc_child->write_path_count; i++) {
                        cfg.extra_mounts[j].path = sc_child->write_paths[i];
                        cfg.extra_mounts[j].ro = 0;
                        j++;
                    }
                    cfg.extra_mount_count = n_extra;
                }
            }
        } else {
            cfg.sandbox = 1;  /* no config → sandbox still required */
        }
        if (sandbox_child_setup(&cfg) != 0)
            _exit(126);

        /* Inject secrets into the child env — but only those this command
         * actually references via $CCLAW_SECRET_<NAME> (least privilege).
         * Runs after sandbox_child_setup so a clean-env scrub can't wipe them.
         * Secrets used via {{SECRET:NAME}} are already interpolated into the
         * command (argv) before the handler runs, so they need no env var;
         * injecting every scoped secret would let `cat /proc/self/environ`
         * dump the agent's whole credential set from any command. */
        if (sc_child) {
            for (size_t i = 0; i < sc_child->secret_count; i++) {
                if (!command_uses_secret_env(command, sc_child->secrets[i].name))
                    continue;
                char envname[256];
                snprintf(envname, sizeof(envname), "CCLAW_SECRET_%s", sc_child->secrets[i].name);
                setenv(envname, sc_child->secrets[i].value, 1);
            }
        }

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* Parent: ensure child is in its own process group */
    setpgid(pid, pid);
    close(pipefd[1]);
    /* The parsed command may carry interpolated {{SECRET}} plaintext. The
     * sandbox child already holds its own copy, so wipe ours from broker memory
     * now — before the proxy relay thread and the drain loop run. */
    for (int i = 0; i < ta.nstrs; i++)
        if (ta.strs[i]) explicit_bzero(ta.strs[i], strlen(ta.strs[i]));
    tool_parse_free(&ta);
    /* Sandbox fork is done — now safe to start the proxy accept thread. */
    if (proxy_active) {
        if (proxy_serve(&proxy) != 0) {
            proxy_stop(&proxy);
            proxy_active = 0;
        }
    }

    char *output = malloc(SHELL_MAX_OUTPUT + 1);
    if (!output) {
        close(pipefd[0]);
        kill(-pid, SIGKILL);
        waitpid(pid, NULL, 0);
        if (proxy_active) proxy_stop(&proxy);
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
        if (proxy_active) proxy_stop(&proxy);
        output[out_len] = '\0';
        /* Secrets are masked by tool_result_postprocess() in the parent — the
         * single masking chokepoint. The handler returns raw output. */
        size_t needed = out_len + 128;
        char *result = malloc(needed);
        if (!result) { free(output); return strdup("error: timeout + OOM"); }
        snprintf(result, needed, "[timeout after %ds]\n%s", timeout, output);
        free(output);
        return result;
    }

    waitpid(pid, &status, 0);

format_result:
    if (proxy_active) proxy_stop(&proxy);
    output[out_len] = '\0';

    /* Secrets are masked by tool_result_postprocess() in the parent — the
     * single masking chokepoint. The handler returns raw output. */
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
    sc->cwd_path = NULL;
    sc->db_path = NULL;
    sc->allowed_hosts = NULL;
    sc->allowed_host_count = 0;
    sc->secrets = NULL;
    sc->secret_count = 0;
    sc->sandbox = 1;
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

/* V88: Collect CCLAW_SECRET_* env vars, clear from environment */
ShellSecret *shell_secrets_collect(size_t *count) {
    extern char **environ;
    *count = 0;

    /* Count matching vars */
    size_t n = 0;
    for (int i = 0; environ[i]; i++) {
        if (strncmp(environ[i], "CCLAW_SECRET_", 13) == 0)
            n++;
    }
    if (n == 0) return NULL;

    ShellSecret *secrets = calloc(n, sizeof(ShellSecret));
    if (!secrets) return NULL;

    size_t idx = 0;
    for (int i = 0; environ[i] && idx < n; ) {
        if (strncmp(environ[i], "CCLAW_SECRET_", 13) == 0) {
            char *eq = strchr(environ[i], '=');
            if (eq) {
                size_t namelen = (size_t)(eq - environ[i]) - 13;
                secrets[idx].name = strndup(environ[i] + 13, namelen);
                secrets[idx].value = strdup(eq + 1);
                idx++;
                /* Unset from env (modifies environ, don't increment i) */
                char key[256];
                size_t klen = (size_t)(eq - environ[i]);
                if (klen < sizeof(key)) {
                    memcpy(key, environ[i], klen);
                    key[klen] = '\0';
                    unsetenv(key);
                } else {
                    i++;
                }
            } else {
                i++;
            }
        } else {
            i++;
        }
    }
    *count = idx;
    return secrets;
}

void shell_secrets_free(ShellSecret *secrets, size_t count) {
    if (!secrets) return;
    for (size_t i = 0; i < count; i++) {
        free(secrets[i].name);
        /* Wipe the plaintext value before releasing it back to the heap. */
        if (secrets[i].value) {
            explicit_bzero(secrets[i].value, strlen(secrets[i].value));
            free(secrets[i].value);
        }
    }
    free(secrets);
}

