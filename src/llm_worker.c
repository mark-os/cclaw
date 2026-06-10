#define _POSIX_C_SOURCE 200809L
#include "llm_worker.h"
#include "llm_proc.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include <curl/curl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* ── Parent-side state ─────────────────────────────────────────── */

static int g_ping_pipe[2] = {-1, -1};  /* parent writes 1 byte, worker reads */
static int g_res_pipe[2] = {-1, -1};   /* worker writes session_id, parent reads */
static pid_t g_worker_pid = -1;
static char g_db_path[4096];
static int g_max_threads = 4;

/* ── Work item (claimed from DB) ───────────────────────────────── */

typedef struct {
    int64_t job_id;
    int64_t session_id;
    char agent_name[64];
    int recall;
} WorkItem;

/* ── Per-thread CURL handle pool (endpoint → CURL*) ────────────── */

#define HANDLE_POOL_CAP 8

typedef struct {
    char *endpoint;
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
    if (p->count >= HANDLE_POOL_CAP) {
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
    WorkItem items[QUEUE_CAP];
    int head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int shutdown;
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

static int wq_pop(WorkQueue *q, WorkItem *out) {
    pthread_mutex_lock(&q->mtx);
    q->idle_threads++;

    while (q->count == 0 && !q->shutdown) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += IDLE_TIMEOUT_SEC;
        int rc = pthread_cond_timedwait(&q->cond, &q->mtx, &ts);
        if (rc != 0 && q->count == 0) {
            if (q->active_threads > 1) {
                q->idle_threads--;
                q->active_threads--;
                pthread_mutex_unlock(&q->mtx);
                return -2;  /* timeout, thread exits */
            }
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

static int wq_push(WorkQueue *q, const WorkItem *item) {
    pthread_mutex_lock(&q->mtx);
    if (q->count >= QUEUE_CAP) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }
    q->items[q->tail] = *item;
    q->tail = (q->tail + 1) % QUEUE_CAP;
    q->count++;
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
    return 0;
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
    if (!db) { fprintf(stderr, "worker thread: db_open failed\n"); return NULL; }
    db_set_child_pragmas(db);

    HandlePool handles;
    handle_pool_init(&handles);

    WorkItem item;
    while (wq_pop(q, &item) == 0) {
        Config *cfg = config_load(db);
        const char *endpoint = cfg ? cfg->provider.base_url : "default";
        CURL *curl = handle_pool_get(&handles, endpoint);
        config_free(cfg);

        llm_req(db, curl, item.session_id, item.recall);

        /* Delete completed job */
        sqlite3_stmt *del;
        if (sqlite3_prepare_v2(db, "DELETE FROM llm_jobs WHERE id=?", -1, &del, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(del, 1, item.job_id);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }

        (void)write(q->result_fd, &item.session_id, sizeof(item.session_id));
    }

    handle_pool_free(&handles);
    db_close(db);
    return NULL;
}

/* ── Dispatcher: reads ping pipe, claims jobs from DB ──────────── */

static void dispatcher_loop(int ping_fd, WorkQueue *queue, const char *db_path) {
    sqlite3 *db = db_open(db_path);
    if (!db) { fprintf(stderr, "dispatcher: db_open failed\n"); return; }
    db_set_child_pragmas(db);

    const char *claim_sql =
        "UPDATE llm_jobs SET status='active', claimed_at=unixepoch()"
        " WHERE id = (SELECT id FROM llm_jobs WHERE status='pending' ORDER BY id LIMIT 1)"
        " RETURNING id, session_id, agent_name, recall";

    struct pollfd pfd = { .fd = ping_fd, .events = POLLIN };

    while (1) {
        int rc = poll(&pfd, 1, 1000);
        if (rc < 0) break;

        if (pfd.revents & (POLLHUP | POLLERR)) break; /* parent died */

        if (pfd.revents & POLLIN) {
            /* Drain ping bytes */
            char drain[64];
            while (read(ping_fd, drain, sizeof(drain)) > 0) {}
        }

        /* Try to claim all pending jobs */
        while (1) {
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db, claim_sql, -1, &stmt, NULL) != SQLITE_OK) break;
            if (sqlite3_step(stmt) != SQLITE_ROW) {
                sqlite3_finalize(stmt);
                break;
            }
            WorkItem item;
            item.job_id = sqlite3_column_int64(stmt, 0);
            item.session_id = sqlite3_column_int64(stmt, 1);
            const char *aname = (const char *)sqlite3_column_text(stmt, 2);
            snprintf(item.agent_name, sizeof(item.agent_name), "%s", aname ? aname : "default");
            item.recall = sqlite3_column_int(stmt, 3);
            sqlite3_finalize(stmt);

            if (wq_push(queue, &item) != 0) {
                /* Queue full — put job back to pending */
                sqlite3_stmt *revert;
                if (sqlite3_prepare_v2(db, "UPDATE llm_jobs SET status='pending', claimed_at=NULL WHERE id=?",
                                       -1, &revert, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(revert, 1, item.job_id);
                    sqlite3_step(revert);
                    sqlite3_finalize(revert);
                }
                int64_t err_sid = -1;
                (void)write(queue->result_fd, &err_sid, sizeof(err_sid));
                break;
            }
        }
    }

    db_close(db);
}

/* ── Worker process main ───────────────────────────────────────── */

static void worker_main(int ping_fd, int res_fd, const char *db_path, int max_threads) {
#ifdef __linux__
    prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
    cclaw_log_init();

#if !defined(__SANITIZE_ADDRESS__) && !defined(__has_feature)
    struct rlimit rl = { .rlim_cur = 512 * 1024 * 1024, .rlim_max = 512 * 1024 * 1024 };
    setrlimit(RLIMIT_AS, &rl);
#endif

    WorkQueue queue;
    wq_init(&queue, max_threads, res_fd, db_path);

    pthread_mutex_lock(&queue.mtx);
    queue.active_threads = 1;
    pthread_mutex_unlock(&queue.mtx);

    pthread_t initial;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&initial, &attr, worker_thread, &queue);
    pthread_attr_destroy(&attr);

    /* Dispatcher runs in main thread — exits on pipe EOF (parent death) */
    dispatcher_loop(ping_fd, &queue, db_path);

    wq_shutdown(&queue);
    sleep(1);
    close(ping_fd);
    close(res_fd);
    _exit(0);
}

/* ── Parent API ────────────────────────────────────────────────── */

int llm_worker_start(const char *db_path, int max_threads) {
    snprintf(g_db_path, sizeof(g_db_path), "%s", db_path);
    g_max_threads = max_threads > 0 ? max_threads : 4;

    if (pipe(g_ping_pipe) != 0) return -1;
    if (pipe(g_res_pipe) != 0) { close(g_ping_pipe[0]); close(g_ping_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) {
        close(g_ping_pipe[0]); close(g_ping_pipe[1]);
        close(g_res_pipe[0]); close(g_res_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(g_ping_pipe[1]);
        close(g_res_pipe[0]);
        worker_main(g_ping_pipe[0], g_res_pipe[1], db_path, g_max_threads);
        _exit(0);
    }

    close(g_ping_pipe[0]);
    close(g_res_pipe[1]);
    g_worker_pid = pid;
    return 0;
}

int llm_worker_submit(sqlite3 *db, int64_t session_id, const char *agent_name, int recall) {
    if (g_ping_pipe[1] < 0) return -1;

    /* INSERT job into DB */
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO llm_jobs(session_id, agent_name, recall) VALUES(?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, agent_name ? agent_name : "default", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, recall);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    /* Ping worker */
    char ping = 1;
    (void)write(g_ping_pipe[1], &ping, 1);
    return 0;
}

int llm_worker_fd(void) { return g_res_pipe[0]; }

int llm_worker_read(int64_t *session_id) {
    ssize_t n = read(g_res_pipe[0], session_id, sizeof(*session_id));
    return (n == (ssize_t)sizeof(*session_id)) ? 0 : -1;
}

void llm_worker_stop(void) {
    if (g_ping_pipe[1] >= 0) { close(g_ping_pipe[1]); g_ping_pipe[1] = -1; }
    if (g_res_pipe[0] >= 0) { close(g_res_pipe[0]); g_res_pipe[0] = -1; }
    if (g_worker_pid > 0) { kill(g_worker_pid, SIGTERM); g_worker_pid = -1; }
}

int llm_worker_respawn(void) {
    llm_worker_stop();
    return llm_worker_start(g_db_path, g_max_threads);
}

int llm_worker_alive(void) { return g_worker_pid > 0; }

pid_t llm_worker_pid(void) { return g_worker_pid; }
