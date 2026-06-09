#define _POSIX_C_SOURCE 200809L
#include "llm_worker.h"
#include "llm_proc.h"
#include "config.h"
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
#include <time.h>
#include <unistd.h>

/* ── Parent-side state ─────────────────────────────────────────── */

static int g_req_pipe[2] = {-1, -1};   /* parent writes, worker reads */
static int g_res_pipe[2] = {-1, -1};   /* worker writes, parent reads */
static pid_t g_worker_pid = -1;
static char g_db_path[4096];
static int g_max_threads = 4;

/* ── Per-thread CURL handle pool (endpoint → CURL*) ────────────── */

#define HANDLE_POOL_CAP 8

typedef struct {
    char *endpoint;   /* "host:port" key */
    CURL *curl;
} HandleEntry;

typedef struct {
    HandleEntry entries[HANDLE_POOL_CAP];
    int count;
} HandlePool;

static void handle_pool_init(HandlePool *p) { memset(p, 0, sizeof(*p)); }

static CURL *handle_pool_get(HandlePool *p, const char *endpoint) {
    for (int i = 0; i < p->count; i++) {
        if (strcmp(p->entries[i].endpoint, endpoint) == 0)
            return p->entries[i].curl;
    }
    /* Create new */
    if (p->count >= HANDLE_POOL_CAP) {
        /* Evict oldest */
        curl_easy_cleanup(p->entries[0].curl);
        free(p->entries[0].endpoint);
        memmove(&p->entries[0], &p->entries[1], (size_t)(p->count - 1) * sizeof(HandleEntry));
        p->count--;
    }
    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    p->entries[p->count].endpoint = strdup(endpoint);
    p->entries[p->count].curl = curl;
    p->count++;
    return curl;
}

static void handle_pool_free(HandlePool *p) {
    for (int i = 0; i < p->count; i++) {
        curl_easy_cleanup(p->entries[i].curl);
        free(p->entries[i].endpoint);
    }
    p->count = 0;
}

/* ── Work queue ────────────────────────────────────────────────── */

#define QUEUE_CAP 64
#define IDLE_TIMEOUT_SEC 30

typedef struct {
    int64_t items[QUEUE_CAP];
    int head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int shutdown;
    /* Elastic pool tracking */
    int active_threads;
    int idle_threads;
    int max_threads;
    int result_fd;
    char db_path[4096];
} WorkQueue;

static void wq_init(WorkQueue *q, int max_threads, int result_fd, const char *db_path) {
    memset(q, 0, sizeof(*q));
    pthread_mutex_init(&q->mtx, NULL);
    pthread_cond_init(&q->cond, NULL);
    q->max_threads = max_threads;
    q->result_fd = result_fd;
    snprintf(q->db_path, sizeof(q->db_path), "%s", db_path);
}

/* Returns 0 if item dequeued, -1 on shutdown, -2 on timeout */
static int wq_pop(WorkQueue *q, int64_t *out) {
    pthread_mutex_lock(&q->mtx);
    q->idle_threads++;

    while (q->count == 0 && !q->shutdown) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += IDLE_TIMEOUT_SEC;
        int rc = pthread_cond_timedwait(&q->cond, &q->mtx, &ts);
        if (rc != 0 && q->count == 0) {
            /* Timeout — thread should exit (unless it's the last one) */
            if (q->active_threads > 1) {
                q->idle_threads--;
                q->active_threads--;
                pthread_mutex_unlock(&q->mtx);
                return -2;
            }
            /* Last thread stays alive */
        }
    }

    q->idle_threads--;
    if (q->shutdown && q->count == 0) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }
    *out = q->items[q->head];
    q->head = (q->head + 1) % QUEUE_CAP;
    q->count--;
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

static void *worker_thread(void *arg);

static void wq_push(WorkQueue *q, int64_t val) {
    pthread_mutex_lock(&q->mtx);
    /* Enqueue */
    if (q->count < QUEUE_CAP) {
        q->items[q->tail] = val;
        q->tail = (q->tail + 1) % QUEUE_CAP;
        q->count++;
    }
    /* Spin up thread if all are busy and below max */
    if (q->idle_threads == 0 && q->active_threads < q->max_threads) {
        q->active_threads++;
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, worker_thread, q);
        pthread_attr_destroy(&attr);
    }
    pthread_cond_signal(&q->cond);
    pthread_mutex_unlock(&q->mtx);
}

static void wq_shutdown(WorkQueue *q) {
    pthread_mutex_lock(&q->mtx);
    q->shutdown = 1;
    pthread_cond_broadcast(&q->cond);
    pthread_mutex_unlock(&q->mtx);
}

/* ── Worker thread ─────────────────────────────────────────────── */

static void *worker_thread(void *arg) {
    WorkQueue *q = arg;
    sqlite3 *db = db_open(q->db_path);
    if (!db) { syslog(LOG_ERR, "worker thread: db_open failed"); return NULL; }
    db_set_child_pragmas(db);

    HandlePool handles;
    handle_pool_init(&handles);

    int64_t session_id;
    int rc;
    while ((rc = wq_pop(q, &session_id)) == 0) {
        /* Determine endpoint from config to select CURL handle */
        Config *cfg = config_load_from_env();
        const char *endpoint = cfg ? cfg->provider.base_url : "default";
        CURL *curl = handle_pool_get(&handles, endpoint);
        config_free(cfg);

        llm_req(db, curl, session_id);
        /* Signal completion */
        (void)write(q->result_fd, &session_id, sizeof(session_id));
    }

    handle_pool_free(&handles);
    db_close(db);
    return NULL;
}

/* ── Worker process main ───────────────────────────────────────── */

static void worker_main(int req_fd, int res_fd, const char *db_path, int max_threads) {
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    cclaw_log_init();

    /* Resource limits — generous for curl+sqlite */
    struct rlimit rl;
#if !defined(__SANITIZE_ADDRESS__) && !defined(__has_feature)
    rl.rlim_cur = 512 * 1024 * 1024;
    rl.rlim_max = 512 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rl);
#endif

    WorkQueue queue;
    wq_init(&queue, max_threads, res_fd, db_path);

    /* Start with 1 thread */
    pthread_mutex_lock(&queue.mtx);
    queue.active_threads = 1;
    pthread_mutex_unlock(&queue.mtx);

    pthread_t initial;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&initial, &attr, worker_thread, &queue);
    pthread_attr_destroy(&attr);

    /* Main thread: read session_ids from parent, enqueue */
    int64_t session_id;
    while (read(req_fd, &session_id, sizeof(session_id)) == (ssize_t)sizeof(session_id)) {
        wq_push(&queue, session_id);
    }

    /* Parent closed pipe — shutdown */
    wq_shutdown(&queue);
    /* Give threads a moment to finish */
    sleep(1);
    close(req_fd);
    close(res_fd);
    _exit(0);
}

/* ── Parent API ────────────────────────────────────────────────── */

int llm_worker_start(const char *db_path, int max_threads) {
    snprintf(g_db_path, sizeof(g_db_path), "%s", db_path);
    g_max_threads = max_threads > 0 ? max_threads : 4;

    if (pipe(g_req_pipe) != 0) return -1;
    if (pipe(g_res_pipe) != 0) { close(g_req_pipe[0]); close(g_req_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(g_req_pipe[0]); close(g_req_pipe[1]);
        close(g_res_pipe[0]); close(g_res_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(g_req_pipe[1]);
        close(g_res_pipe[0]);
        worker_main(g_req_pipe[0], g_res_pipe[1], db_path, g_max_threads);
        _exit(0);
    }

    close(g_req_pipe[0]);
    close(g_res_pipe[1]);
    g_worker_pid = pid;
    return 0;
}

int llm_worker_submit(int64_t session_id) {
    if (g_req_pipe[1] < 0) return -1;
    ssize_t n = write(g_req_pipe[1], &session_id, sizeof(session_id));
    return (n == (ssize_t)sizeof(session_id)) ? 0 : -1;
}

int llm_worker_fd(void) { return g_res_pipe[0]; }

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
    return llm_worker_start(g_db_path, g_max_threads);
}

int llm_worker_alive(void) { return g_worker_pid > 0; }
