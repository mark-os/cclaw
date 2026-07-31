#define _POSIX_C_SOURCE 200809L
#include "tool_thread.h"
#include "db.h"
#include "secret_store.h"
#include "secret_interp.h"
#include "secret_capture.h"
#include "db_response.h"
#include "context.h"
#include "types.h"
#include "unicode_normalize.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* ── Module state ─────────────────────────────────────────────────────── */
static int  g_notify_pipe[2] = {-1, -1};
static char g_db_path[1024];
static int  g_cli_mode = 0;
static int  g_started = 0;

static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static int  g_in_flight = 0;   /* detached threads still running */
static int  g_stopping = 0;

int tool_thread_start(const char *db_path, int cli_mode) {
    if (g_started) return 0;
    if (pipe(g_notify_pipe) != 0) return -1;
    fcntl(g_notify_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_notify_pipe[1], F_SETFD, FD_CLOEXEC);
    fcntl(g_notify_pipe[0], F_SETFL,
          fcntl(g_notify_pipe[0], F_GETFL) | O_NONBLOCK);
    snprintf(g_db_path, sizeof(g_db_path), "%s", db_path);
    g_cli_mode = cli_mode;
    g_in_flight = 0;
    g_stopping = 0;
    g_started = 1;
    return 0;
}

int tool_thread_fd(void) { return g_notify_pipe[0]; }

int tool_thread_read(int64_t *session_id) {
    ssize_t n = read(g_notify_pipe[0], session_id, sizeof(*session_id));
    return (n == (ssize_t)sizeof(*session_id)) ? 0 : -1;
}

/* ── Thread body ──────────────────────────────────────────────────────── */
/* Owns `job` (heap copy) and job->args; frees both before exit. Opens its OWN
 * sqlite3* (the registry handler's captured db belongs to the poll thread —
 * SQLite handles are not shared across threads) and writes the result through
 * it. The AFTER_TOOL_CALL observer hook is deliberately NOT run here: it evals
 * JS on the shared extension runtime, which is unsafe off the poll thread, and
 * it targets the agent's externally-visible tools (file/shell/web/js) — those
 * run on the sandbox path and keep the hook in reap_children. */
static void *tool_thread_fn(void *arg) {
    ToolThreadJob *job = (ToolThreadJob *)arg;
    sqlite3 *db = db_open(g_db_path);
    if (db) db_set_child_pragmas(db);

    char *result = NULL;
    if (db && job->run)
        result = job->run(db, job->agent_name, job->session_id, job->args);
    if (!result)
        result = strdup(db ? "error: tool returned null" : "error: tool thread db_open failed");

    /* Postprocess: deinterpolate {{SECRET}} + secret scan/redact (scan
     * runs even with no secrets — tool output can carry leaked credentials).
     * job->secrets is the immutable env-only base; merge in a fresh DB read
     * through THIS thread's own db handle so a secret born mid-session is
     * maskable here too (never share the dispatch-scoped snapshot — it
     * wouldn't outlive the async thread). */
    if (result && db) {
        char *cap = secret_capture_apply(db, job->args, result);
        if (cap) { free(result); result = cap; }
    }
    if (result) {
        size_t snap_n = 0;
        ShellSecret *snap = db ? secrets_snapshot(db, job->secrets, job->secret_count, &snap_n)
                               : NULL;
        char *pp = tool_result_postprocess(result, snap ? snap : job->secrets,
                                           snap ? snap_n : job->secret_count);
        if (snap) secrets_snapshot_free(snap, snap_n);
        if (pp) { free(result); result = pp; }
    }
    if (!result) result = strdup("error: OOM");

    if (g_cli_mode) {
        size_t rlen = strlen(result);
        if (rlen <= 80) fprintf(stdout, "\033[2m\xe2\x86\x92 %s\033[0m\n", result);
        else            fprintf(stdout, "\033[2m\xe2\x86\x92 %.77s...\033[0m\n", result);
        fflush(stdout);
    }

    if (db) {
        /* Ensure valid UTF-8 before DB storage */
        { char *clean = utf8_sanitize(result, strlen(result));
          if (clean) { free(result); result = clean; } }

        char *stored = truncate_and_spill(result, job->session_id, job->tool_call_id);
        ToolResult tr = {.tool_call_id = job->tool_call_id,
                         .content = stored ? stored : result};
        int is_err = (strncmp(result, "error:", 6) == 0);
        Message msg = {.role = ROLE_TOOL, .tool_result = &tr,
                       .tool_name = job->tool_name, .is_error = is_err};
        int64_t rid = entry_append_with_iteration(db, job->session_id, &msg, job->iteration_id);
        db_tool_call_complete_with_result(db, job->entry_id, job->tool_call_id, rid);
        free(stored);
        db_close(db);
    }
    free(result);
    free(job->args);

    int64_t sid = job->session_id;
    free(job);

    /* Wake the poll loop to advance the session (run_advance runs there). */
    (void)write(g_notify_pipe[1], &sid, sizeof(sid));

    /* Accounting: single exit point. Wake the drain barrier when the last
     * in-flight thread leaves. */
    pthread_mutex_lock(&g_mtx);
    g_in_flight--;
    if (g_in_flight == 0)
        pthread_cond_broadcast(&g_cond);
    pthread_mutex_unlock(&g_mtx);
    return NULL;
}

int tool_thread_spawn(const ToolThreadJob *job) {
    if (!g_started || !job) { if (job) free(job->args); return -1; }

    ToolThreadJob *copy = malloc(sizeof(*copy));
    if (!copy) { free(job->args); return -1; }
    *copy = *job;   /* shallow: takes ownership of args; secrets borrowed */

    pthread_mutex_lock(&g_mtx);
    if (g_stopping) { pthread_mutex_unlock(&g_mtx); free(copy->args); free(copy); return -1; }
    g_in_flight++;
    pthread_mutex_unlock(&g_mtx);

    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int cr = pthread_create(&t, &attr, tool_thread_fn, copy);
    pthread_attr_destroy(&attr);
    if (cr != 0) {
        pthread_mutex_lock(&g_mtx);
        g_in_flight--;
        if (g_in_flight == 0) pthread_cond_broadcast(&g_cond);
        pthread_mutex_unlock(&g_mtx);
        free(copy->args);
        free(copy);
        return -1;
    }
    return 0;
}

void tool_thread_stop(void) {
    if (!g_started) return;
    pthread_mutex_lock(&g_mtx);
    g_stopping = 1;
    while (g_in_flight > 0)
        pthread_cond_wait(&g_cond, &g_mtx);
    pthread_mutex_unlock(&g_mtx);

    if (g_notify_pipe[0] >= 0) { close(g_notify_pipe[0]); g_notify_pipe[0] = -1; }
    if (g_notify_pipe[1] >= 0) { close(g_notify_pipe[1]); g_notify_pipe[1] = -1; }
    g_started = 0;
}
