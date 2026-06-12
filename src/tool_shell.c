#define _GNU_SOURCE
#include "tool_shell.h"
#include "tool_parse.h"
#include "preload_blob.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define SHELL_MAX_OUTPUT (256 * 1024)

/* V82/V37/V22a: Apply namespace sandbox in shell child.
 * unshare(USER|MNT|PID|NET), write uid/gid maps, pivot_root into minimal fs.
 * System dirs mounted ro, workspace rw, cwd_path rw (CLI mode).
 * Mount setup happens before the PID namespace fork.
 * Returns 0 on success, -1 on failure (caller should log + continue). */
static int shell_apply_namespace(const char *workspace, const char *cwd_path) {
    uid_t uid = getuid();
    gid_t gid = getgid();

    /* Resolve paths before pivot_root changes the root */
    char ws_abs[PATH_MAX];
    const char *ws_resolved = NULL;
    if (workspace && workspace[0]) {
        if (realpath(workspace, ws_abs))
            ws_resolved = ws_abs;
    }

    char cwd_abs[PATH_MAX];
    const char *cwd_resolved = NULL;
    if (cwd_path && cwd_path[0]) {
        if (realpath(cwd_path, cwd_abs))
            cwd_resolved = cwd_abs;
    }

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWNET) != 0)
        return -1;

    /* Write uid_map: map ns root (0) to our real uid */
    int fd = open("/proc/self/uid_map", O_WRONLY);
    if (fd < 0) return -1;
    char map[64];
    int len = snprintf(map, sizeof(map), "0 %u 1\n", uid);
    int ok = (write(fd, map, (size_t)len) == len);
    close(fd);
    if (!ok) return -1;

    /* Deny setgroups (required before writing gid_map on unprivileged) */
    fd = open("/proc/self/setgroups", O_WRONLY);
    if (fd >= 0) {
        ok = (write(fd, "deny\n", 5) == 5);
        close(fd);
        if (!ok) return -1;
    }

    /* Write gid_map */
    fd = open("/proc/self/gid_map", O_WRONLY);
    if (fd < 0) return -1;
    len = snprintf(map, sizeof(map), "0 %u 1\n", gid);
    ok = (write(fd, map, (size_t)len) == len);
    close(fd);
    if (!ok) return -1;

    /* Make entire mount tree private so changes don't propagate */
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0)
        return -1;

    /* Create tmpfs as new root */
    char newroot[] = "/tmp/.cclaw_ns_XXXXXX";
    if (!mkdtemp(newroot))
        return -1;
    if (mount("none", newroot, "tmpfs", MS_NOSUID | MS_NODEV, "size=1m") != 0)
        return -1;

    /* Bind-mount system dirs read-only */
    static const char *sys_dirs[] = {
        "/bin", "/usr", "/lib", "/lib64", "/etc", "/proc", "/dev", "/sbin", NULL
    };
    for (int i = 0; sys_dirs[i]; i++) {
        struct stat st;
        if (stat(sys_dirs[i], &st) != 0) continue;
        char dst[256];
        snprintf(dst, sizeof(dst), "%s%s", newroot, sys_dirs[i]);
        mkdir(dst, 0755);
        if (mount(sys_dirs[i], dst, NULL, MS_BIND | MS_REC, NULL) == 0) {
            mount(NULL, dst, NULL,
                  MS_REMOUNT | MS_BIND | MS_REC | MS_RDONLY | MS_NOSUID, NULL);
        }
    }

    /* Create /tmp in new root (ephemeral tmpfs, writable for shell scratch) */
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", newroot);
    mkdir(tmp_path, 01777);

    /* Bind-mount workspace rw */
    if (ws_resolved) {
        struct stat st;
        if (stat(ws_resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
            char ws_dst[PATH_MAX + 64];
            snprintf(ws_dst, sizeof(ws_dst), "%s%s", newroot, ws_resolved);
            char *p = ws_dst + strlen(newroot) + 1;
            for (char *slash = p; *slash; slash++) {
                if (*slash == '/') {
                    *slash = '\0';
                    mkdir(ws_dst, 0755);
                    *slash = '/';
                }
            }
            mkdir(ws_dst, 0755);
            mount(ws_resolved, ws_dst, NULL, MS_BIND, NULL);
        }
    }

    /* Bind-mount CWD rw (CLI mode) */
    if (cwd_resolved) {
        struct stat st;
        if (stat(cwd_resolved, &st) == 0 && S_ISDIR(st.st_mode)) {
            char cwd_dst[PATH_MAX + 64];
            snprintf(cwd_dst, sizeof(cwd_dst), "%s%s", newroot, cwd_resolved);
            char *p = cwd_dst + strlen(newroot) + 1;
            for (char *slash = p; *slash; slash++) {
                if (*slash == '/') {
                    *slash = '\0';
                    mkdir(cwd_dst, 0755);
                    *slash = '/';
                }
            }
            mkdir(cwd_dst, 0755);
            mount(cwd_resolved, cwd_dst, NULL, MS_BIND, NULL);
        }
    }

    /* pivot_root into new root */
    char put_old[256];
    snprintf(put_old, sizeof(put_old), "%s/.oldroot", newroot);
    mkdir(put_old, 0755);
    if (syscall(SYS_pivot_root, newroot, put_old) != 0)
        return -1;

    chdir("/");
    umount2("/.oldroot", MNT_DETACH);
    rmdir("/.oldroot");

    /* CLONE_NEWPID requires a fork — the child becomes PID 1 in the new
     * PID namespace. Mount setup is already done, so the fork+exec is clean. */
    pid_t inner = fork();
    if (inner < 0) return -1;
    if (inner > 0) {
        /* Parent of inner fork: wait and propagate exit status */
        int st;
        waitpid(inner, &st, 0);
        _exit(WIFEXITED(st) ? WEXITSTATUS(st) : 254);
    }

    /* PID 1 in new namespace: remount /proc for correct PID view */
    mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

    /* CWD into workspace so shell commands start there */
    if (ws_resolved && chdir(ws_resolved) != 0)
        chdir("/tmp");

    return 0;
}

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

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
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

        /* V82/V37: namespace sandbox — required unless trust_level is host */
        const char *ws = user_data ? ((ShellConfig *)user_data)->workspace : NULL;
        const char *cwd = user_data ? ((ShellConfig *)user_data)->cwd_path : NULL;
        const char *psock = user_data ? ((ShellConfig *)user_data)->proxy_sock : NULL;
        int do_sandbox = user_data ? ((ShellConfig *)user_data)->sandbox : 1;
        ShellConfig *sc_child = user_data ? (ShellConfig *)user_data : NULL;

        /* Trust-level: suppress CWD mount if mount_cwd=0 */
        if (sc_child && !sc_child->mount_cwd) cwd = NULL;
        /* Trust-level: suppress proxy if net_mode=1 */
        if (sc_child && sc_child->net_mode) psock = NULL;

        if (do_sandbox && shell_apply_namespace(ws, cwd) != 0) {
            /* Fail closed: a runtime failure must not grant what only
             * trust_level=host may grant (stderr = captured in output). */
            fprintf(stderr, "error: namespace sandbox unavailable (errno=%d); "
                    "this trust level requires it — enable unprivileged user "
                    "namespaces or set the agent's trust_level to 'host'\n", errno);
            _exit(126);
        }

        /* Trust-level: remount workspace read-only if workspace_ro=1 */
        if (do_sandbox && sc_child && sc_child->workspace_ro && ws) {
            /* After pivot_root, workspace is at its resolved path */
            char ws_real[PATH_MAX];
            if (realpath(ws, ws_real) == NULL ||
                mount(NULL, ws_real, NULL, MS_REMOUNT | MS_BIND | MS_RDONLY, NULL) != 0) {
                fprintf(stderr, "error: read-only workspace remount failed "
                        "(errno=%d); refusing to run writable\n", errno);
                _exit(126);
            }
        }

        /* V47: PATH restriction + env hardening */
        if (sc_child && sc_child->env_mode == 1) {
            /* Clean allowlist: wipe entire env, set only essentials */
            extern char **environ;
            if (environ) environ[0] = NULL;  /* portable clearenv */
            setenv("PATH", "/bin:/usr/bin", 1);
            setenv("TMPDIR", "/tmp", 1);
        } else {
            /* Legacy trusted mode: inherit env minus known secret names */
            setenv("PATH", "/bin:/usr/bin", 1);
            unsetenv("HOME");
            extern char **environ;
            char *drop_keys[256];
            int nkeys = 0;
            for (int i = 0; environ[i] && nkeys < 256; i++) {
                char *eq = strchr(environ[i], '=');
                if (!eq) continue;
                size_t klen = (size_t)(eq - environ[i]);
                char name[256];
                if (klen >= sizeof(name)) continue;
                memcpy(name, environ[i], klen);
                name[klen] = '\0';
                if (strncmp(name, "CCLAW_", 6) == 0 ||
                    strstr(name, "API_KEY") || strstr(name, "APIKEY") ||
                    strstr(name, "TOKEN") || strstr(name, "SECRET") ||
                    strstr(name, "PASSWORD") || strstr(name, "CREDENTIALS")) {
                    drop_keys[nkeys] = strdup(name);
                    if (drop_keys[nkeys]) nkeys++;
                }
            }
            for (int i = 0; i < nkeys; i++) {
                unsetenv(drop_keys[i]);
                free(drop_keys[i]);
            }
        }

        /* V83: set proxy socket path for LD_PRELOAD lib (after env cleanup) */
        if (psock) setenv("CCLAW_PROXY_SOCK", psock, 1);

        /* V88: inject secrets into shell child env */
        if (sc_child) {
            for (size_t i = 0; i < sc_child->secret_count; i++) {
                char envname[256];
                snprintf(envname, sizeof(envname), "CCLAW_SECRET_%s", sc_child->secrets[i].name);
                setenv(envname, sc_child->secrets[i].value, 1);
            }
        }

        /* Trust-level: apply resource limits before exec */
        if (sc_child && sc_child->rlimits.nproc > 0) {
            struct rlimit rl = {(rlim_t)sc_child->rlimits.nproc, (rlim_t)sc_child->rlimits.nproc};
            setrlimit(RLIMIT_NPROC, &rl);
        }
        if (sc_child && sc_child->rlimits.as_mb > 0) {
            rlim_t bytes = (rlim_t)sc_child->rlimits.as_mb * 1024 * 1024;
            struct rlimit rl = {bytes, bytes};
            setrlimit(RLIMIT_AS, &rl);
        }
        if (sc_child && sc_child->rlimits.cpu_sec > 0) {
            struct rlimit rl = {(rlim_t)sc_child->rlimits.cpu_sec, (rlim_t)sc_child->rlimits.cpu_sec};
            setrlimit(RLIMIT_CPU, &rl);
        }

        /* Write preload lib into the namespace's private /tmp and set
         * LD_PRELOAD — the interposer is the only egress inside CLONE_NEWNET,
         * so a failed write must be loud, not silently no-network. */
        if (psock && do_sandbox) {
            int pfd = open("/tmp/libcclaw_net.so", O_WRONLY | O_CREAT | O_TRUNC, 0755);
            ssize_t wr = pfd >= 0 ? write(pfd, preload_net_blob, (size_t)preload_net_blob_len) : -1;
            if (pfd >= 0) close(pfd);
            if (wr == (ssize_t)preload_net_blob_len)
                setenv("LD_PRELOAD", "/tmp/libcclaw_net.so", 1);
            else
                fprintf(stderr, "[cclaw] warning: proxy preload setup failed "
                        "(errno=%d); shell has no network\n", errno);
        }

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* Parent: ensure child is in its own process group */
    setpgid(pid, pid);
    close(pipefd[1]);
    tool_parse_free(&ta);

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
        /* V88: mask secrets in timeout output */
        if (user_data) {
            ShellConfig *sc3 = (ShellConfig *)user_data;
            shell_mask_secrets(output, &out_len, SHELL_MAX_OUTPUT, sc3->secrets, sc3->secret_count);
        }
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

    /* V88: mask secret values in output before returning */
    if (user_data) {
        ShellConfig *sc3 = (ShellConfig *)user_data;
        shell_mask_secrets(output, &out_len, SHELL_MAX_OUTPUT, sc3->secrets, sc3->secret_count);
    }

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
    sc->proxy_sock = NULL;
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
        free(secrets[i].value);
    }
    free(secrets);
}

/* Replace all occurrences of needle in output with tag (in-place) */
static void mask_replace(char *output, size_t *len, size_t cap,
                         const char *needle, size_t nlen,
                         const char *tag, size_t taglen) {
    char *p = output;
    while ((p = memmem(p, *len - (size_t)(p - output), needle, nlen)) != NULL) {
        size_t offset = (size_t)(p - output);
        size_t tail = *len - offset - nlen;

        if (taglen <= nlen) {
            memcpy(p, tag, taglen);
            memmove(p + taglen, p + nlen, tail + 1);
            *len = *len - nlen + taglen;
        } else if (*len - nlen + taglen < cap) {
            memmove(p + taglen, p + nlen, tail + 1);
            memcpy(p, tag, taglen);
            *len = *len - nlen + taglen;
        } else {
            memcpy(p, tag, taglen);
            *len = offset + taglen;
            output[*len] = '\0';
            break;
        }
        p = output + offset + taglen;
    }
}

/* Base64 encode src into dst. Returns bytes written (no NUL). */
static size_t b64_encode(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t needed = ((src_len + 2) / 3) * 4;
    if (needed > dst_cap) return 0;
    size_t o = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        unsigned char a = (unsigned char)src[i];
        unsigned char b = (i + 1 < src_len) ? (unsigned char)src[i + 1] : 0;
        unsigned char c = (i + 2 < src_len) ? (unsigned char)src[i + 2] : 0;
        dst[o++] = t[a >> 2];
        dst[o++] = t[((a & 3) << 4) | (b >> 4)];
        dst[o++] = (i + 1 < src_len) ? t[((b & 0xf) << 2) | (c >> 6)] : '=';
        dst[o++] = (i + 2 < src_len) ? t[c & 0x3f] : '=';
    }
    return o;
}

/* URL-encode src into dst. Returns bytes written (no NUL). */
static size_t url_encode(char *dst, size_t dst_cap, const char *src, size_t src_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < src_len; i++) {
        unsigned char ch = (unsigned char)src[i];
        int safe = ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                    (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                    ch == '.' || ch == '~');
        if (safe) {
            if (o + 1 > dst_cap) return 0;
            dst[o++] = (char)ch;
        } else {
            if (o + 3 > dst_cap) return 0;
            dst[o++] = '%';
            dst[o++] = hex[ch >> 4];
            dst[o++] = hex[ch & 0xf];
        }
    }
    return o;
}

/* V88: Replace secret values in output with [REDACTED:<name>].
 * Scans for exact match + base64 + URL-encoded variants. */
void shell_mask_secrets(char *output, size_t *len, size_t cap,
                        const ShellSecret *secrets, size_t secret_count) {
    if (!secrets || secret_count == 0 || !output) return;

    for (size_t s = 0; s < secret_count; s++) {
        const char *val = secrets[s].value;
        size_t vlen = strlen(val);
        if (vlen == 0) continue;

        char tag[280];
        int tlen = snprintf(tag, sizeof(tag), "[REDACTED:%s]", secrets[s].name);
        if (tlen < 0 || (size_t)tlen >= sizeof(tag)) continue;
        size_t taglen = (size_t)tlen;

        /* Exact match */
        mask_replace(output, len, cap, val, vlen, tag, taglen);

        /* Base64-encoded variant */
        char b64[1024];
        size_t b64len = b64_encode(b64, sizeof(b64), val, vlen);
        if (b64len > 0 && b64len != vlen) /* skip if same as raw (unlikely) */
            mask_replace(output, len, cap, b64, b64len, tag, taglen);

        /* URL-encoded variant (only if it differs from raw) */
        char urlenc[2048];
        size_t urllen = url_encode(urlenc, sizeof(urlenc), val, vlen);
        if (urllen > 0 && urllen != vlen)
            mask_replace(output, len, cap, urlenc, urllen, tag, taglen);
    }
}
