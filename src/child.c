#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "child.h"

#include "proc.h"
#include "run_tool.h"
#include "util.h"

#define TOOL_MAX_OUTPUT (60 * 1024)  /* 60KB — fits in pipe buffer */

/* ── Child tracking ─────────────────────────────────────────────── */

static ChildProc g_children[CHILD_MAX];
static int g_child_count;

/* Iteration surface for the scans that live outside this module. */
int child_count(void) {
    return g_child_count;
}

ChildProc *child_at(int idx) {
    if (idx < 0 || idx >= g_child_count) return NULL;
    return &g_children[idx];
}

ChildProc *child_find(pid_t pid) {
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].pid == pid) return &g_children[i];
    return NULL;
}

void child_remove(ChildProc *c) {
    int idx = (int)(c - g_children);
    if (idx < 0 || idx >= g_child_count) return;
    if (c->result_pipe >= 0) {
        close(c->result_pipe);
        c->result_pipe = -1;
    }
    free(c->outbuf);
    c->outbuf = NULL;
    c->outbuf_len = 0;
    free(c->hosts_json);
    c->hosts_json = NULL;
    free(c->tool_args);
    c->tool_args = NULL;
    free(c->cron_prompt);
    c->cron_prompt = NULL;
    g_children[idx] = g_children[g_child_count - 1];
    g_child_count--;
}

/* Both --run-tool child kinds: forked, sandboxed, answering over a result
 * pipe. Everything that polls or drains those pipes covers both. */
static int child_is_run_tool(const ChildProc *c) {
    return c->type == CHILD_TOOL_EXEC || c->type == CHILD_CRON_SCRIPT;
}

int child_has_session(int64_t session_id) {
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].type == CHILD_TOOL_EXEC
            && g_children[i].session_id == session_id)
            return 1;
    return 0;
}

/* ── SIGCHLD self-pipe ──────────────────────────────────────────── */

static int g_chld_pipe[2] = {-1, -1};

static void sigchld_handler(int sig) {
    (void)sig;
    char c = 1;
    (void)write(g_chld_pipe[1], &c, 1);
}

int child_sigchld_init(void) {
    if (pipe(g_chld_pipe) != 0) { perror("pipe"); return -1; }
    fcntl(g_chld_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_chld_pipe[1], F_SETFD, FD_CLOEXEC);
    util_set_nonblock(g_chld_pipe[0]);
    util_set_nonblock(g_chld_pipe[1]);
    { struct sigaction sa = {0}; sa.sa_handler = sigchld_handler;
      sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
      sigaction(SIGCHLD, &sa, NULL); }
    return 0;
}

int child_sigchld_fd(void) {
    return g_chld_pipe[0];
}

void child_sigchld_teardown(void) {
    /* Disarm SIGCHLD before closing the self-pipe: a child reaped after the
     * close would otherwise have the handler write into a reused fd. */
    signal(SIGCHLD, SIG_DFL);
    close(g_chld_pipe[0]); close(g_chld_pipe[1]);
}

/* ── Pipe draining helpers ─────────────────────────────────────── */

/* Largest hosts-JSON meta accepted from a child; larger metas are drained and
 * discarded (hosts_json stays NULL) so a misbehaving child can't balloon us. */
#define FRAME_META_MAX (64 * 1024)

/* Drain a tool child's result pipe (nonblocking): parse the frame header +
 * hosts meta, then accumulate the result body into c->outbuf, kept
 * NUL-terminated. Body bytes beyond TOOL_MAX_OUTPUT are read and discarded so
 * the child never blocks on a full pipe. Closes the fd on EOF or error;
 * leaves it open on EAGAIN (more data may come). */
void child_drain_pipe(ChildProc *c) {
    if (c->result_pipe < 0) return;

    char buf[4096];
    ssize_t n;
    while ((n = read(c->result_pipe, buf, sizeof(buf))) > 0) {
        size_t off = 0;

        /* Frame header: 4-byte network-order meta length, may arrive split. */
        while (c->frame_hdr_read < 4 && off < (size_t)n)
            c->frame_hdr[c->frame_hdr_read++] = (unsigned char)buf[off++];
        if (c->frame_hdr_read == 4 && c->frame_meta_read == 0 && !c->hosts_json) {
            c->frame_meta_len = ((size_t)c->frame_hdr[0] << 24) |
                                ((size_t)c->frame_hdr[1] << 16) |
                                ((size_t)c->frame_hdr[2] << 8)  |
                                 (size_t)c->frame_hdr[3];
            if (c->frame_meta_len > 0 && c->frame_meta_len <= FRAME_META_MAX &&
                !c->hosts_json)
                c->hosts_json = calloc(1, c->frame_meta_len + 1);
        }
        if (c->frame_hdr_read < 4) continue;

        /* Meta bytes (hosts JSON); oversized meta is consumed but dropped. */
        while (c->frame_meta_read < c->frame_meta_len && off < (size_t)n) {
            size_t want = c->frame_meta_len - c->frame_meta_read;
            size_t avail = (size_t)n - off;
            size_t take = want < avail ? want : avail;
            if (c->hosts_json)
                memcpy(c->hosts_json + c->frame_meta_read, buf + off, take);
            c->frame_meta_read += take;
            off += take;
        }
        if (off >= (size_t)n) continue;

        /* Result body. */
        size_t to_copy = (size_t)n - off;
        if (c->outbuf_len + to_copy > TOOL_MAX_OUTPUT)
            to_copy = TOOL_MAX_OUTPUT - c->outbuf_len;
        if (to_copy == 0) continue; /* at cap: keep draining, discard */
        char *tmp = realloc(c->outbuf, c->outbuf_len + to_copy + 1);
        if (!tmp) continue; /* OOM: drop chunk, keep child unblocked */
        memcpy(tmp + c->outbuf_len, buf + off, to_copy);
        c->outbuf = tmp;
        c->outbuf_len += to_copy;
        c->outbuf[c->outbuf_len] = '\0';
    }
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        close(c->result_pipe);
        c->result_pipe = -1;
    }
}

/* Append every live tool result pipe to a pollfd set being rebuilt. */
int add_result_pipe_fds(struct pollfd *pfds, int nfds, int max) {
    for (int i = 0; i < g_child_count && nfds < max; i++) {
        if (child_is_run_tool(&g_children[i]) && g_children[i].result_pipe >= 0) {
            pfds[nfds].fd = g_children[i].result_pipe;
            pfds[nfds].events = POLLIN;
            nfds++;
        }
    }
    return nfds;
}

/* Drain whichever result pipes poll() reported readable. */
void drain_ready_result_pipes(const struct pollfd *pfds, int base, int nfds) {
    for (int i = base; i < nfds; i++) {
        if (!(pfds[i].revents & (POLLIN | POLLHUP))) continue;
        for (int j = 0; j < g_child_count; j++) {
            if (child_is_run_tool(&g_children[j]) &&
                g_children[j].result_pipe == pfds[i].fd) {
                child_drain_pipe(&g_children[j]);
                break;
            }
        }
    }
}

/* ── spawn_run_tool_child: fork+exec a --run-tool child ──────────── */

#define FD_REQUEST RUNTOOL_FD_REQUEST  /* the socketpair fd in the child */

/* Spawn a sandboxed --run-tool child via fork+execve and register it. The
 * request blob is sent over a socketpair (fd 3 in the child). Returns the
 * child's slot (caller fills the kind-specific fields) or NULL if the ceiling
 * is reached, the socketpair fails, or the fork/write fails. */
ChildProc *spawn_run_tool_blob(ChildType type, int64_t session_id,
                               const char *agent_name, const char *blob,
                               size_t blob_len, int timeout_sec) {
    if (g_child_count >= CHILD_MAX) return NULL;

    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) return NULL;

    /* sp[0] = parent side, sp[1] = child side (becomes fd 3) */
    /* Set O_CLOEXEC on parent side so it doesn't leak into other children */
    fcntl(sp[0], F_SETFD, FD_CLOEXEC);
    /* Child side must NOT have CLOEXEC (it becomes fd 3 post-dup2) */

    pid_t pid = fork();
    if (pid < 0) {
        close(sp[0]); close(sp[1]);
        return NULL;
    }
    if (pid == 0) {
        /* CHILD: async-signal-safe only. No malloc, no stdio, no snprintf. */
        close(sp[0]);
        /* dup2 child socket to fd 3 */
        if (sp[1] != FD_REQUEST) {
            dup2(sp[1], FD_REQUEST);
            close(sp[1]);
        }
        /* execve self as --run-tool. Minimal env (inherits nothing sensitive). */
        char *const argv[] = {"cclaw", "--run-tool", NULL};
        char *const envp[] = {proc_log_level_env(), NULL};
        execve("/proc/self/exe", argv, envp);
        /* execve failed — write a framed static error (4-byte zero meta_len
         * header, then the message) to fd 3 and die */
        (void)write(FD_REQUEST, "\0\0\0\0error: execve failed", 24);
        _exit(127);
    }

    /* PARENT: close child end, write request blob (blocking, safe because
     * blob is capped at RUNTOOL_REQUEST_MAX < kernel socket buffer) */
    close(sp[1]);
    ssize_t written = 0;
    size_t total = blob_len;
    while ((size_t)written < total) {
        ssize_t w = write(sp[0], blob + written, total - (size_t)written);
        if (w <= 0) {
            close(sp[0]);
            waitpid(pid, NULL, 0);
            return NULL;
        }
        written += w;
    }

    /* Switch to nonblocking for result reads in poll loop */
    util_set_nonblock(sp[0]);

    /* Register child — mirrors dispatch_tool bookkeeping */
    ChildProc *c = &g_children[g_child_count++];
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->type = type;
    c->session_id = session_id;
    c->result_pipe = sp[0];
    c->timeout_sec = timeout_sec > 0 ? timeout_sec : 120;
    c->deadline = time(NULL) + c->timeout_sec;
    snprintf(c->agent_name, sizeof(c->agent_name), "%s", agent_name ? agent_name : "");
    return c;
}

/* Spawn a sandboxed tool child. Returns 0 on success (child is registered in
 * g_children), -1 on failure (caller writes the error result inline). */
int spawn_run_tool_child(int64_t session_id, const char *agent_name,
                         const char *tool_call_id, const char *tool_name,
                         const char *tool_args, int64_t iteration_id,
                         int64_t entry_id, const char *blob, size_t blob_len,
                         int timeout_sec) {
    ChildProc *c = spawn_run_tool_blob(CHILD_TOOL_EXEC, session_id, agent_name,
                                       blob, blob_len, timeout_sec);
    if (!c) return -1;
    c->iteration_id = iteration_id;
    c->entry_id = entry_id;
    snprintf(c->tool_call_id, sizeof(c->tool_call_id), "%s", tool_call_id);
    snprintf(c->tool_name, sizeof(c->tool_name), "%s", tool_name);
    c->tool_args = tool_args ? strdup(tool_args) : NULL;
    return 0;
}
