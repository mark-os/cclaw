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
#include <sys/uio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "child.h"
#include "llm_bridge.h"

#include "proc.h"
#include "run_tool.h"
#include "util.h"

/* The most result body the parent keeps from one child. It is the wire limit
 * itself: a child never sends more (run_tool.h), and a smaller cap here cut
 * the "full output is in <file>" note off the end of a spilled result. */
#define TOOL_MAX_OUTPUT RUNTOOL_RESULT_MAX

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

ChildProc *child_find_tool_call(int64_t session_id, const char *call_id) {
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].type == CHILD_TOOL_EXEC &&
            g_children[i].session_id == session_id &&
            call_id && strcmp(g_children[i].tool_call_id, call_id) == 0)
            return &g_children[i];
    return NULL;
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
    free(c->hook_path);
    c->hook_path = NULL;
    if (c->log_fd >= 0) {
        close(c->log_fd);
        c->log_fd = -1;
    }
    llm_bridge_stop(c->llm_bridge);
    c->llm_bridge = NULL;
    g_children[idx] = g_children[g_child_count - 1];
    g_child_count--;
}

/* The --run-tool child kinds: forked, sandboxed, answering over a result
 * pipe. Everything that polls or drains those pipes covers all of them. */
static int child_is_run_tool(const ChildProc *c) {
    return c->type == CHILD_TOOL_EXEC || c->type == CHILD_CRON_SCRIPT ||
           c->type == CHILD_HOOK;
}

void child_hook_unhold(int64_t session_id) {
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].type == CHILD_HOOK
            && g_children[i].session_id == session_id)
            g_children[i].hook_wait = 0;
}

int child_hook_holds(int64_t session_id) {
    /* deadline -1 = swept: the timeout has been dealt with and the slot is
     * only waiting to be reaped, so it holds nothing any more. */
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].type == CHILD_HOOK && g_children[i].hook_wait
            && g_children[i].session_id == session_id
            && g_children[i].deadline != -1)
            return 1;
    return 0;
}

int child_has_session(int64_t session_id) {
    /* "Turn tool work in flight" — every caller uses this to hold the turn
     * (no LLM dispatch, no advance, CLI prompt stays busy). A background
     * job's child is exactly NOT turn work: its call was answered at
     * dispatch and the session must keep advancing while it runs. */
    for (int i = 0; i < g_child_count; i++)
        if (g_children[i].type == CHILD_TOOL_EXEC
            && g_children[i].session_id == session_id
            && !g_children[i].background)
            return 1;
    return 0;
}

/* What a child that could not exec says instead of a tool result: a complete
 * frame in the format child_drain_pipe() parses (status byte, 4-byte meta
 * length, body). Static so the post-fork path stays async-signal-safe. */
const unsigned char child_exec_fail_frame[] =
    "\001"              /* RUNTOOL_STATUS_ERROR */
    "\0\0\0\0"          /* no hosts meta */
    "error: execve failed";
const size_t child_exec_fail_frame_len = sizeof(child_exec_fail_frame) - 1;

/* ── SIGCHLD self-pipe ──────────────────────────────────────────── */

static int g_chld_pipe[2] = {-1, -1};

static void sigchld_handler(int sig) {
    (void)sig;
    /* A full self-pipe makes write() fail with EAGAIN; the code this handler
     * interrupted must not see that in its errno. */
    int saved = errno;
    char c = 1;
    (void)!write(g_chld_pipe[1], &c, 1);
    errno = saved;
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

/* Drain a tool child's result pipe (nonblocking): parse the frame header
 * (status byte + meta length) + hosts meta, then accumulate the result body into c->outbuf, kept
 * NUL-terminated. Body bytes beyond TOOL_MAX_OUTPUT are read and discarded so
 * the child never blocks on a full pipe. Closes the fd on EOF or error;
 * leaves it open on EAGAIN (more data may come). */
void child_drain_pipe(ChildProc *c) {
    if (c->result_pipe < 0) return;

    char buf[4096];
    ssize_t n;
    while ((n = read(c->result_pipe, buf, sizeof(buf))) > 0) {
        size_t off = 0;

        /* Frame header: status byte + 4-byte network-order meta length; may
         * arrive split across reads. The status is the child's own verdict on
         * the call and rides the same frame as the body it describes. */
        while (c->frame_hdr_read < 5 && off < (size_t)n)
            c->frame_hdr[c->frame_hdr_read++] = (unsigned char)buf[off++];
        if (c->frame_hdr_read == 5 && c->frame_meta_read == 0 && !c->hosts_json) {
            c->frame_status = (c->frame_hdr[0] == RUNTOOL_STATUS_ERROR);
            c->frame_meta_len = ((size_t)c->frame_hdr[1] << 24) |
                                ((size_t)c->frame_hdr[2] << 16) |
                                ((size_t)c->frame_hdr[3] << 8)  |
                                 (size_t)c->frame_hdr[4];
            if (c->frame_meta_len > 0 && c->frame_meta_len <= FRAME_META_MAX &&
                !c->hosts_json)
                c->hosts_json = calloc(1, c->frame_meta_len + 1);
        }
        if (c->frame_hdr_read < 5) continue;

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

        /* Result body. A background job streams to its log file on disk —
         * bounded by disk, not TOOL_MAX_OUTPUT — so the agent can tail the
         * live log from inside the sandbox; the daemon keeps no copy. */
        if (c->log_fd >= 0) {
            size_t rem = (size_t)n - off;
            while (rem > 0) {
                ssize_t w = write(c->log_fd, buf + off, rem);
                if (w <= 0) break;   /* disk error: drop chunk, keep draining */
                off += (size_t)w;
                rem -= (size_t)w;
            }
            continue;
        }
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
    return spawn_run_tool_blob_fd(type, session_id, agent_name, blob, blob_len,
                                  -1, timeout_sec);
}

/* Pass one open descriptor to the peer of a socketpair, riding a single
 * byte. Returns 0 on success. */
static int send_fd(int sock, int fd) {
    char byte = 0;
    union { struct cmsghdr h; char buf[CMSG_SPACE(sizeof(int))]; } ctl;
    memset(&ctl, 0, sizeof(ctl));
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                          .msg_control = ctl.buf,
                          .msg_controllen = sizeof(ctl.buf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    ssize_t n;
    do { n = sendmsg(sock, &msg, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    return n == 1 ? 0 : -1;
}

ChildProc *spawn_run_tool_blob_fd(ChildType type, int64_t session_id,
                                  const char *agent_name, const char *blob,
                                  size_t blob_len, int arg_fd, int timeout_sec) {
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
        char *const envp[] = {proc_log_level_env(), proc_js_heap_env(), NULL};
        execve("/proc/self/exe", argv, envp);
        /* execve failed — write a framed static error to fd 3 and die */
        (void)!write(FD_REQUEST, child_exec_fail_frame, child_exec_fail_frame_len);
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
    /* The request's TOOL_ARG_JSON_FD value, if it has one: the descriptor
     * follows the blob. One byte into an empty socket buffer — it cannot
     * block — and the child reads the file itself, so a value of any size
     * costs the event loop nothing. */
    if (arg_fd >= 0 && send_fd(sp[0], arg_fd) != 0) {
        close(sp[0]);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        return NULL;
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
    c->log_fd = -1;
    c->timeout_sec = timeout_sec > 0 ? timeout_sec : 120;
    c->deadline = time(NULL) + c->timeout_sec;
    snprintf(c->agent_name, sizeof(c->agent_name), "%s", agent_name ? agent_name : "");
    return c;
}

/* Spawn a sandboxed tool child. Returns the registered ChildProc (so the
 * caller can attach per-call state like the LLM bridge), NULL on failure
 * (caller writes the error result inline). */
ChildProc *spawn_run_tool_child(int64_t session_id, const char *agent_name,
                         const char *tool_call_id, const char *tool_name,
                         const char *tool_args, int64_t iteration_id,
                         int64_t entry_id, const char *blob, size_t blob_len,
                         int timeout_sec) {
    ChildProc *c = spawn_run_tool_blob(CHILD_TOOL_EXEC, session_id, agent_name,
                                       blob, blob_len, timeout_sec);
    if (!c) return NULL;
    c->iteration_id = iteration_id;
    c->entry_id = entry_id;
    snprintf(c->tool_call_id, sizeof(c->tool_call_id), "%s", tool_call_id);
    snprintf(c->tool_name, sizeof(c->tool_name), "%s", tool_name);
    c->tool_args = tool_args ? strdup(tool_args) : NULL;
    return c;
}
