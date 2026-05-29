#define _GNU_SOURCE
#include "tool_shell.h"
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

#define SHELL_MAX_OUTPUT (256 * 1024)

/* V82/V37: Apply namespace sandbox in shell child.
 * unshare(USER|MNT|NET), write uid/gid maps, pivot_root into minimal fs.
 * System dirs mounted ro, workspace rw.
 * Returns 0 on success, -1 on failure (caller should log + continue). */
static int shell_apply_namespace(const char *workspace) {
    uid_t uid = getuid();
    gid_t gid = getgid();

    if (unshare(CLONE_NEWUSER | CLONE_NEWNS | CLONE_NEWNET) != 0)
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
    if (workspace && workspace[0]) {
        struct stat st;
        if (stat(workspace, &st) == 0 && S_ISDIR(st.st_mode)) {
            char ws_dst[512];
            snprintf(ws_dst, sizeof(ws_dst), "%s%s", newroot, workspace);
            /* Create parent dirs for workspace path */
            char *p = ws_dst + strlen(newroot) + 1;
            for (char *slash = p; *slash; slash++) {
                if (*slash == '/') {
                    *slash = '\0';
                    mkdir(ws_dst, 0755);
                    *slash = '/';
                }
            }
            mkdir(ws_dst, 0755);
            mount(workspace, ws_dst, NULL, MS_BIND, NULL);
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

        /* V82/V37: namespace sandbox — graceful fallback if unavailable */
        const char *ws = user_data ? ((ShellConfig *)user_data)->workspace : NULL;
        const char *psock = user_data ? ((ShellConfig *)user_data)->proxy_sock : NULL;
        if (shell_apply_namespace(ws) != 0) {
            /* Fallback: continue unsandboxed (log to stderr = captured in output) */
            fprintf(stderr, "[cclaw] warning: namespace sandbox unavailable, "
                    "continuing without (errno=%d)\n", errno);
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

        /* V83: set proxy socket path for LD_PRELOAD lib (after CCLAW_* cleanup) */
        if (psock) setenv("CCLAW_PROXY_SOCK", psock, 1);

        /* V88: inject secrets into shell child env */
        if (user_data) {
            ShellConfig *sc2 = (ShellConfig *)user_data;
            for (size_t i = 0; i < sc2->secret_count; i++) {
                char envname[256];
                snprintf(envname, sizeof(envname), "CCLAW_SECRET_%s", sc2->secrets[i].name);
                setenv(envname, sc2->secrets[i].value, 1);
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
    sc->proxy_sock = NULL;
    sc->secrets = NULL;
    sc->secret_count = 0;
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
