#ifndef CCLAW_CHILD_H
#define CCLAW_CHILD_H

/*
 * Forked children: the table that tracks them, the SIGCHLD self-pipe that
 * notices them dying, the result-pipe drain, and the fork+exec of a
 * sandboxed --run-tool child.
 *
 * Mechanism only. This module spawns, tracks, and drains bytes; what a
 * finished child's output *means* — a tool result, a cron result, a timeout
 * message — belongs to the dispatch side, which reads the table through the
 * accessors below rather than touching it directly.
 *
 * Lib-resident: loop.c's run_advance asks child_has_session() whether a
 * session already has a tool child in flight, so child.o cannot live in
 * main.o (which the Makefile filters out of libcclaw.a).
 */

#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* Ceiling on live children — also the poll-set sizing and the stalled-session
 * set's bound. */
#define CHILD_MAX 48

/* CHILD_CRON_SCRIPT is a --run-tool child like CHILD_TOOL_EXEC — same fork,
 * same JS tier, same result pipe — answering a cron fire instead of a model
 * tool call, so its completion posts a cron_result (or an inbox row) rather
 * than a tool result. */
typedef enum { CHILD_CHANNEL, CHILD_TOOL_EXEC, CHILD_CRON_SCRIPT } ChildType;

typedef struct {
    pid_t pid;
    ChildType type;
    int64_t session_id;
    char agent_name[64];
    /* LLM_REQ fields */
    int iteration;
    /* TOOL_EXEC fields */
    char tool_call_id[64];
    char tool_name[64];     /* for the §8 observer hook */
    char *tool_args;        /* strdup'd args for the observer; freed on cleanup */
    int64_t iteration_id;
    int64_t entry_id;       /* tool_call entry id */
    int result_pipe;        /* read end of pipe for tool output */
    char *outbuf;           /* Accumulator for tool output (grows with realloc) */
    size_t outbuf_len;      /* Bytes currently in outbuf */
    /* fd-3 frame parse state: [4-byte meta_len (network order)][hosts JSON]
     * [result to EOF]. The pipe is non-blocking and poll-driven, so header
     * and meta can arrive fragmented across drains. */
    size_t frame_hdr_read;      /* bytes of the 4-byte header consumed */
    unsigned char frame_hdr[4];
    size_t frame_meta_len;      /* parsed meta length (valid once hdr_read==4) */
    size_t frame_meta_read;     /* meta bytes consumed so far */
    char *hosts_json;           /* meta payload; NULL if absent/oversized */
    /* Channel fields */
    char channel_name[64];
    char binary_path[512];
    int restart_count;
    /* CRON_SCRIPT fields */
    char cron_job[80];      /* job name — the result's source_ref / data.job */
    char *cron_prompt;      /* 'both' payload: rides one inbox row with the
                             * script output; NULL for a script-only fire */
    /* Deadline: 0 = no timeout, >0 = SIGKILL after this time */
    time_t deadline;
    int timeout_sec;        /* window used for `deadline`, for the timeout message */
} ChildProc;

/* ── The table ──────────────────────────────────────────────────────
 * Storage is module-private; these are the whole cross-TU surface. The
 * count/at pair is the iterator for the two scans that live outside this
 * module (the poll timeout and the deadline sweep) — slots are unordered and
 * child_remove() moves the last slot into the removed one, so a loop that
 * removes must not advance past the slot it just cleared. */
int child_count(void);
ChildProc *child_at(int idx);

ChildProc *child_find(pid_t pid);
void child_remove(ChildProc *c);

/* 1 if a tool child is already running for this session. */
int child_has_session(int64_t session_id);

/* ── SIGCHLD self-pipe ──────────────────────────────────────────────
 * The handler writes one byte; the poll loop drains child_sigchld_fd() and
 * reaps. init returns 0, or -1 after reporting the failure. */
int child_sigchld_init(void);
int child_sigchld_fd(void);
void child_sigchld_teardown(void);

/* ── Result pipes ───────────────────────────────────────────────────── */

/* Drain a child's result pipe (nonblocking) into its outbuf. */
void child_drain_pipe(ChildProc *c);

/* Append every live result pipe to a pollfd set being rebuilt; returns the
 * new nfds. */
int add_result_pipe_fds(struct pollfd *pfds, int nfds, int max);

/* Drain whichever result pipes poll() reported readable. */
void drain_ready_result_pipes(const struct pollfd *pfds, int base, int nfds);

/* ── Spawning ───────────────────────────────────────────────────────── */

/* Fork+exec a sandboxed --run-tool child around `blob` and register it.
 * Returns its slot (caller fills the kind-specific fields) or NULL. */
ChildProc *spawn_run_tool_blob(ChildType type, int64_t session_id,
                               const char *agent_name, const char *blob,
                               size_t blob_len, int timeout_sec);

/* spawn_run_tool_blob wrapper for a model tool call: 0 on success, -1 on
 * failure (caller writes the error result inline). */
int spawn_run_tool_child(int64_t session_id, const char *agent_name,
                         const char *tool_call_id, const char *tool_name,
                         const char *tool_args, int64_t iteration_id,
                         int64_t entry_id, const char *blob, size_t blob_len,
                         int timeout_sec);

#endif
