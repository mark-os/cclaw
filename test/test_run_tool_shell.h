/* test_run_tool_shell.h — shared helper for tests that drive shell_exec via the
 * real --run-tool re-exec path (fork+execve), matching production behavior.
 *
 * Usage:
 *   ShellToolReq r = SHELL_REQ_DEFAULTS;
 *   r.command = "echo hello";
 *   r.workspace = "/tmp/my_workspace";
 *   char *result = run_tool_shell(&r);
 *   // result is "[exit 0]\nhello\n" or "[timeout after Ns]\n..."
 *   free(result);
 */
#ifndef CCLAW_TEST_RUN_TOOL_SHELL_H
#define CCLAW_TEST_RUN_TOOL_SHELL_H

#include "run_tool.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHELL_BINARY "./build/cclaw"

/* Request config for shell tier. Fields map 1:1 to RunToolReq but only expose
 * what shell tests need. Initialize with SHELL_REQ_DEFAULTS for sane values. */
typedef struct {
    const char *command;
    const char *shell_path;     /* NULL = /bin/sh */
    const char *workspace;
    const char *agent_dir;      /* dir for proxy UDS (NULL = use workspace) */
    const char *cwd_path;       /* CWD rw bind-mount (NULL = none) */
    const char *db_path;        /* cclaw.db: key + ciphertext masked in the ns */
    const char *tmp_dir;        /* host dir bound as /tmp (NULL = tiny root tmpfs) */
    int timeout;
    int sandbox;
    int env_mode;               /* 1 = clean env (default), 0 = inherit */
    int mount_cwd;
    int net_mode;               /* 1 = no network (no proxy), 0 = proxy */
    const char *spill_path;     /* oversized output goes here (NULL = none) */
    int nproc, as_mb, cpu_sec;
    const char **host_rules;
    size_t host_count;
    const char **read_paths;
    size_t read_count;
    const char **write_paths;
    size_t write_count;
    const RunToolSecret *secrets;
    size_t secret_count;
    const char *egress_note;    /* parent-composed proxy-deny advice */
} ShellToolReq;

#define SHELL_REQ_DEFAULTS { \
    .command = NULL, .shell_path = NULL, .workspace = NULL, .agent_dir = NULL, \
    .cwd_path = NULL, .db_path = NULL, .tmp_dir = NULL, \
    .timeout = 5, .sandbox = 1, .env_mode = 1, \
    .mount_cwd = 0, .net_mode = 0, \
    .nproc = 64, .as_mb = 512, .cpu_sec = 60, \
    .host_rules = NULL, .host_count = 0, \
    .read_paths = NULL, .read_count = 0, \
    .write_paths = NULL, .write_count = 0, \
    .secrets = NULL, .secret_count = 0, \
}

/* Send a shell-tier request via the real re-exec path and unpack the whole
 * fd-3 frame: [status][4-byte BE meta_len][meta][result].
 * *out_hosts (optional) gets the hosts JSON, *out_status (optional) the
 * child's explicit tool status (0 ok, 1 error). Caller frees. */
static inline char *run_tool_shell_full(const ShellToolReq *r, char **out_hosts,
                                        int *out_status) {
    if (out_hosts) *out_hosts = NULL;
    if (out_status) *out_status = 0;

    RunToolReq req = {
        .tier = RUNTOOL_TIER_SHELL,
        .tool_name = "shell_exec",
        .env_mode = r->env_mode,
        .nproc = r->nproc,
        .as_mb = r->as_mb,
        .cpu_sec = r->cpu_sec,
        .sandbox = r->sandbox,
        .net_mode = r->net_mode,
        .spill_path = r->spill_path,
        .workspace = r->workspace,
        .cwd_path = r->cwd_path,
        .db_path = r->db_path,
        .tmp_dir = r->tmp_dir,
        .mount_cwd = r->mount_cwd,
        .read_paths = r->read_paths,
        .read_count = r->read_count,
        .write_paths = r->write_paths,
        .write_count = r->write_count,
        .agent_dir = r->agent_dir ? r->agent_dir : r->workspace,
        .host_rules = r->host_rules,
        .host_count = r->host_count,
        .command = r->command,
        .shell_path = r->shell_path,
        .timeout = r->timeout,
        .secrets = r->secrets,
        .secret_count = r->secret_count,
        .egress_note = r->egress_note,
    };

    size_t blob_len = 0;
    char *blob = run_tool_serialize_request(&req, &blob_len);
    if (!blob) return strdup("error: serialize failed");

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
        free(blob);
        return strdup("error: socketpair failed");
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(blob); close(sp[0]); close(sp[1]);
        return strdup("error: fork failed");
    }
    if (pid == 0) {
        close(sp[0]);
        if (sp[1] != 3) { dup2(sp[1], 3); close(sp[1]); }
        char *const argv[] = {"cclaw", "--run-tool", NULL};
        char *const envp[] = {NULL};
        execve(SHELL_BINARY, argv, envp);
        _exit(127);
    }

    close(sp[1]);
    size_t written = 0;
    while (written < blob_len) {
        ssize_t w = write(sp[0], blob + written, blob_len - written);
        if (w <= 0) break;
        written += (size_t)w;
    }
    free(blob);
    shutdown(sp[0], SHUT_WR);

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    assert(buf);
    for (;;) {
        if (len + 1024 > cap) { cap *= 2; buf = realloc(buf, cap); assert(buf); }
        ssize_t n = read(sp[0], buf + len, cap - len);
        if (n <= 0) break;
        len += (size_t)n;
    }
    close(sp[0]);
    buf[len] = '\0';

    int status;
    waitpid(pid, &status, 0);

    if (len < 5) { free(buf); return strdup("error: short frame"); }
    if (out_status) *out_status = (buf[0] == RUNTOOL_STATUS_ERROR);
    size_t meta_len = ((size_t)(unsigned char)buf[1] << 24) |
                      ((size_t)(unsigned char)buf[2] << 16) |
                      ((size_t)(unsigned char)buf[3] << 8) |
                       (size_t)(unsigned char)buf[4];
    if (5 + meta_len > len) { free(buf); return strdup("error: bad frame"); }
    if (out_hosts && meta_len > 0)
        *out_hosts = strndup(buf + 5, meta_len);
    char *result = strdup(buf + 5 + meta_len);
    free(buf);
    return result;
}

/* Result text only. */
static inline char *run_tool_shell(const ShellToolReq *r) {
    return run_tool_shell_full(r, NULL, NULL);
}

/* Result text + hosts JSON metadata. */
static inline char *run_tool_shell_with_hosts(const ShellToolReq *r, char **out_hosts) {
    return run_tool_shell_full(r, out_hosts, NULL);
}

/* Convenience: detect namespace availability */
static inline int run_tool_ns_available(const char *workspace) {
    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "echo ns_check";
    r.workspace = workspace;
    char *res = run_tool_shell(&r);
    int avail = (res && strstr(res, "ns_check") != NULL);
    free(res);
    return avail;
}

#endif /* CCLAW_TEST_RUN_TOOL_SHELL_H */
