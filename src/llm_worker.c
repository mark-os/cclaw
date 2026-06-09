#define _POSIX_C_SOURCE 200809L
#include "llm_worker.h"
#include "llm_proc.h"
#include "db.h"
#include "log.h"
#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

/* ── Parent-side state ─────────────────────────────────────────── */

static int g_req_pipe[2] = {-1, -1};   /* parent writes, worker reads */
static int g_res_pipe[2] = {-1, -1};   /* worker writes, parent reads */
static pid_t g_worker_pid = -1;
static char g_db_path[4096];
static int g_num_threads = 2;

/* ── Worker-side thread pool ───────────────────────────────────── */

#define QUEUE_CAP 64

typedef struct {
    int64_t items[QUEUE_CAP];
    int head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int shutdown;
} WorkQueue;

static void wq_init(WorkQueue *q) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cond, NULL);
}

static void wq_push(WorkQueue *q, int64_t val) {
    pthread_mutex_lock(&q->mtx);
    while (q->count >= QUEUE_CAP && !q->shutdown)
        pthread_cond_wait(&q->cond, &q->mtx);
    if (!q->shutdown) {
        q->items[q->tail] = val;
        q->tail = (q->tail + 1) % QUEUE_CAP;
        q->count++;
    }
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mtx);
}

static int wq_pop(WorkQueue *q, int64_t *out) {
    pthread_mutex_lock(&q->mtx);
    while (q->count == 0 && !q->shutdown)
        pthread_cond_wait(&q->cond, &q->mtx);
    if (q->shutdown && q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }
    *out = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

static void wq_shutdown(WorkQueue *q) {
    pthread_mutex_lock(&q->mtx);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mtx);
}

/* Worker thread function */
typedef struct {
    WorkQueue *queue;
    int result_fd;
    char db_path[4096];
} ThreadCtx;

static void *worker_thread(void *arg) {
    ThreadCtx *ctx = arg;
    sqlite3 *db = db_open(ctx->db_path);
    if (!db) { syslog(LOG_ERR, "worker thread: db_open failed"); return NULL; }
    db_set_child_pragmas(db);

    CURL *curl = curl_easy_init();

    int64_t session_id;
    while (wq_pop(ctx->queue, &session_id) == 0) {
        llm_req(db, curl, session_id);
        /* Signal completion to parent */
        (void)write(ctx->result_fd, &session_id, sizeof(session_id));
    }

    if (curl) curl_easy_cleanup(curl);
    db_close(db);
    return NULL;
}

/* Worker main (runs in child process after fork) */
static void worker_main(int req_fd, int res_fd, const char *db_path, int num_threads) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    cclaw_log_init();

    /* Resource limits */
    struct rlimit rl;
#if !defined(__SANITIZE_ADDRESS__) && !defined(__has_feature)
    rl.rlim_cur = 512 * 1024 * 1024;
    rl.rlim_max = 512 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rl);
#endif

    WorkQueue queue;
    wq_init(&queue);

    ThreadCtx *ctxs = calloc((size_t)num_threads, sizeof(ThreadCtx));
    pthread_t *threads = calloc((size_t)num_threads, sizeof(pthread_t));

    for (int i = 0; i < num_threads; i++) {
        ctxs[i].queue = &queue;
        ctxs[i].result_fd = res_fd;
        snprintf(ctxs[i].db_path, sizeof(ctxs[i].db_path), "%s", db_path);
        pthread_create(&threads[i], NULL, worker_thread, &ctxs[i]);
    }

    /* Main thread: read session_ids from parent, enqueue */
    int64_t session_id;
    while (read(req_fd, &session_id, sizeof(session_id)) == (ssize_t)sizeof(session_id)) {
        wq_push(&queue, session_id);
    }

    /* Parent closed pipe — shutdown */
    wq_shutdown(&queue);
    for (int i = 0; i < num_threads; i++)
        pthread_join(threads[i], NULL);

    free(ctxs);
    free(threads);
    close(req_fd);
    close(res_fd);
    _exit(0);
}

/* ── Parent API ────────────────────────────────────────────────── */

int llm_worker_start(const char *db_path, int num_threads) {
    snprintf(g_db_path, sizeof(g_db_path), "%s", db_path);
    g_num_threads = num_threads > 0 ? num_threads : 2;

    if (pipe(g_req_pipe) != 0) return -1;
    if (pipe(g_res_pipe) != 0) { close(g_req_pipe[0]); close(g_req_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(g_req_pipe[0]); close(g_req_pipe[1]);
        close(g_res_pipe[0]); close(g_res_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        /* Child: close parent ends */
        close(g_req_pipe[1]);
        close(g_res_pipe[0]);
        worker_main(g_req_pipe[0], g_res_pipe[1], db_path, g_num_threads);
        _exit(0);
    }

    /* Parent: close child ends */
    close(g_req_pipe[0]);
    close(g_res_pipe[1]);
    g_worker_pid = pid;
    return 0;
}

int llm_worker_submit(int64_t session_id) {
    if (g_req_pipe[1] < 0) return -1;
    ssize_t n = write(g_req_pipe[1], &session_id, sizeof(session_id));
    return (n == sizeof(session_id)) ? 0 : -1;
}

int llm_worker_fd(void) {
    return g_res_pipe[0];
}

int llm_worker_read(int64_t *session_id) {
    ssize_t n = read(g_res_pipe[0], session_id, sizeof(*session_id));
    return (n == (ssize_t)sizeof(*session_id)) ? 0 : -1;
}

void llm_worker_stop(void) {
    if (g_req_pipe[1] >= 0) { close(g_req_pipe[1]); g_req_pipe[1] = -1; }
    if (g_res_pipe[0] >= 0) { close(g_res_pipe[0]); g_res_pipe[0] = -1; }
    if (g_worker_pid > 0) { kill(g_worker_pid, SIGTERM); g_worker_pid = -1; }
}

int llm_worker_respawn(void) {
    llm_worker_stop();
    return llm_worker_start(g_db_path, g_num_threads);
}

int llm_worker_alive(void) {
    return g_worker_pid > 0;
}
