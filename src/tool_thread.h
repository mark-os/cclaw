#ifndef CCLAW_TOOL_THREAD_H
#define CCLAW_TOOL_THREAD_H

#include <sqlite3.h>
#include <stdint.h>
#include "secret_interp.h"   /* ShellSecret */

/* Fire-and-forget tool threads (EXEC_THREAD vehicle).
 *
 * Separate from the LLM worker pool (llm_worker.c): that pool *reuses* threads
 * to amortize the curl/TLS handshake to the provider. DB-only tools get no such
 * benefit, so each call spawns its own detached thread that runs to completion
 * and exits — no queue, no reuse, no head-of-line blocking behind LLM streams.
 *
 * The poll loop never runs tool logic: it dispatches, the thread does the work
 * + writes the result through its OWN sqlite3* (the registry handler's captured
 * db is the poll thread's — never touched here), then writes the session_id to
 * a notify pipe the loop drains exactly like the worker-notify fd → run_advance.
 */

typedef struct {
    int64_t session_id;
    int64_t iteration_id;
    int64_t entry_id;
    char    tool_call_id[128];
    char    tool_name[64];
    char    agent_name[64];
    char   *args;                 /* owned; freed by the thread */
    /* Rebuilds the tool's ctx around the thread's db + agent_name (+ session_id)
     * and runs the handler. Returns a heap result string (caller frees). */
    char  *(*run)(sqlite3 *db, const char *agent_name,
                  int64_t session_id, const char *args);
    const ShellSecret *secrets;   /* borrowed (immutable post-setup); for postprocess */
    size_t  secret_count;
} ToolThreadJob;

/* Create the notify pipe + in-flight accounting. cli_mode==1 prints the dim
 * "→ result" progress line like the inline/sandbox paths. Returns 0 on success. */
int  tool_thread_start(const char *db_path, int cli_mode);

/* Read end of the notify pipe — add to the poll set. */
int  tool_thread_fd(void);

/* Drain one completion notification. Returns 0 and sets *session_id, or -1. */
int  tool_thread_read(int64_t *session_id);

/* Spawn a detached thread for job. Takes ownership of job->args (and frees it
 * even on failure). Returns 0 on success. */
int  tool_thread_spawn(const ToolThreadJob *job);

/* Drain barrier: block until all in-flight tool threads finish, then close the
 * pipe. Mirrors llm_worker_stop — a detached thread racing teardown and touching
 * freed state was the rare exit SIGSEGV. */
void tool_thread_stop(void);

#endif
