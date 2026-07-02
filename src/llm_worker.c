#define _POSIX_C_SOURCE 200809L
#include "llm_worker.h"
#include "llm_proc.h"
#include "config.h"
#include "db.h"
#include "log.h"
#include <curl/curl.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

/* ── Configuration ─────────────────────────────────────────────── */

#define QUEUE_CAP       64
#define IDLE_TIMEOUT_S  30

/* ── Work item ─────────────────────────────────────────────────── */

typedef struct {
    int64_t job_id;
    int64_t session_id;
    char agent_name[64];
    int recall;
} WorkItem;

/* ── Thread pool ───────────────────────────────────────────────── */

static struct {
    WorkItem items[QUEUE_CAP];
    int head, tail, count;
    pthread_mutex_t mtx;
    pthread_cond_t cond;
    int shutdown;
    int active, idle, max;
    int notify_fd;          /* write end — wakes parent poll loop */
    char db_path[4096];
} g_pool;

static int g_notify_pipe[2] = {-1, -1};
static int g_started;

/* ── Queue ops ─────────────────────────────────────────────────── */

static void *worker_fn(void *arg);

static int pool_push(const WorkItem *item) {
    pthread_mutex_lock(&g_pool.mtx);
    if (g_pool.count >= QUEUE_CAP) {
        pthread_mutex_unlock(&g_pool.mtx);
        return -1;
    }
    g_pool.items[g_pool.tail] = *item;
    g_pool.tail = (g_pool.tail + 1) % QUEUE_CAP;
    g_pool.count++;

    /* Spawn thread if none idle */
    if (g_pool.idle == 0 && g_pool.active < g_pool.max) {
        g_pool.active++;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, worker_fn, NULL) != 0)
            g_pool.active--;   /* rollback: no thread reached the exit point */
        pthread_attr_destroy(&attr);
    }
    pthread_cond_signal(&g_pool.cond);
    pthread_mutex_unlock(&g_pool.mtx);
    return 0;
}

static int pool_pop(WorkItem *out) {
    pthread_mutex_lock(&g_pool.mtx);
    g_pool.idle++;

    while (g_pool.count == 0 && !g_pool.shutdown) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += IDLE_TIMEOUT_S;
        int rc = pthread_cond_timedwait(&g_pool.cond, &g_pool.mtx, &ts);
        if (rc != 0 && g_pool.count == 0 && g_pool.active > 1) {
            /* Idle timeout — shrink pool. active-- happens in worker_fn's
             * single exit point so the drain barrier in llm_worker_stop()
             * accounts for every thread exactly once. */
            g_pool.idle--;
            pthread_mutex_unlock(&g_pool.mtx);
            return -1;
        }
    }

    g_pool.idle--;
    if (g_pool.shutdown && g_pool.count == 0) {
        pthread_mutex_unlock(&g_pool.mtx);
        return -1;
    }
    *out = g_pool.items[g_pool.head];
    g_pool.head = (g_pool.head + 1) % QUEUE_CAP;
    g_pool.count--;
    pthread_mutex_unlock(&g_pool.mtx);
    return 0;
}

/* ── Worker thread ─────────────────────────────────────────────── */

static void *worker_fn(void *arg) {
    (void)arg;
    sqlite3 *db = db_open(g_pool.db_path);
    if (!db) {
        cclaw_log_write(LOG_ERR, "worker: db_open failed");
        pthread_mutex_lock(&g_pool.mtx);
        g_pool.active--;
        if (g_pool.active == 0)
            pthread_cond_broadcast(&g_pool.cond);
        pthread_mutex_unlock(&g_pool.mtx);
        return NULL;
    }
    db_set_child_pragmas(db);

    CURL *curl = curl_easy_init();

    WorkItem item;
    while (pool_pop(&item) == 0) {
        /* Determine job type from DB */
        int job_type = 0;
        sqlite3_stmt *qstmt;
        if (sqlite3_prepare_v2(db, "SELECT job_type FROM llm_jobs WHERE id=?",
                               -1, &qstmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(qstmt, 1, item.job_id);
            if (sqlite3_step(qstmt) == SQLITE_ROW)
                job_type = sqlite3_column_int(qstmt, 0);
            sqlite3_finalize(qstmt);
        }

        /* Tag this worker thread's log lines (including llm_req's) with the
         * session and agent for the duration of the job. */
        cclaw_log_set_ctx(item.session_id, -1, item.agent_name);

        if (job_type == 1) {
            /* Compaction job */
            cclaw_log_write(LOG_DEBUG, "worker: compaction start");
            llm_compaction(db, curl, item.session_id, item.agent_name);
        } else {
            /* LLM turn job */
            cclaw_log_write(LOG_DEBUG, "worker: model start");
            llm_req(db, curl, item.session_id, item.recall);
        }

        cclaw_log_clear_ctx();

        /* Clean up job record */
        sqlite3_stmt *del;
        if (sqlite3_prepare_v2(db, "DELETE FROM llm_jobs WHERE id=?",
                               -1, &del, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(del, 1, item.job_id);
            sqlite3_step(del);
            sqlite3_finalize(del);
        }

        /* Notify parent poll loop */
        (void)write(g_pool.notify_fd, &item.session_id, sizeof(item.session_id));
    }

    if (curl) curl_easy_cleanup(curl);
    db_close(db);

    /* Single exit point for thread accounting. Decrement active and, when the
     * last worker leaves, wake llm_worker_stop()'s drain wait. */
    pthread_mutex_lock(&g_pool.mtx);
    g_pool.active--;
    if (g_pool.active == 0)
        pthread_cond_broadcast(&g_pool.cond);
    pthread_mutex_unlock(&g_pool.mtx);
    return NULL;
}

/* ── Public API ────────────────────────────────────────────────── */

int llm_worker_start(const char *db_path, int max_threads) {
    if (g_started) return 0;

    if (pipe(g_notify_pipe) != 0) return -1;
    fcntl(g_notify_pipe[0], F_SETFD, FD_CLOEXEC);
    fcntl(g_notify_pipe[1], F_SETFD, FD_CLOEXEC);
    /* Read end non-blocking for parent poll */
    fcntl(g_notify_pipe[0], F_SETFL,
          fcntl(g_notify_pipe[0], F_GETFL) | O_NONBLOCK);

    memset(&g_pool, 0, sizeof(g_pool));
    pthread_mutex_init(&g_pool.mtx, NULL);
    pthread_cond_init(&g_pool.cond, NULL);
    g_pool.max = max_threads > 0 ? max_threads : 4;
    g_pool.notify_fd = g_notify_pipe[1];
    snprintf(g_pool.db_path, sizeof(g_pool.db_path), "%s", db_path);

    /* Pre-start one thread — fail closed if it can't be created */
    g_pool.active = 1;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int cr = pthread_create(&t, &attr, worker_fn, NULL);
    pthread_attr_destroy(&attr);
    if (cr != 0) {
        g_pool.active = 0;
        pthread_mutex_destroy(&g_pool.mtx);
        pthread_cond_destroy(&g_pool.cond);
        close(g_notify_pipe[0]); g_notify_pipe[0] = -1;
        close(g_notify_pipe[1]); g_notify_pipe[1] = -1;
        return -1;
    }

    g_started = 1;
    return 0;
}

int llm_worker_submit(sqlite3 *db, int64_t session_id,
                      const char *agent_name, int recall) {
    if (!g_started) return -1;

    /* Persist job (crash recovery) */
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO llm_jobs(session_id, agent_name, recall) VALUES(?,?,?)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, agent_name ? agent_name : "Assistant", -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, recall);
    int rc = sqlite3_step(stmt);
    int64_t job_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    /* Push to thread pool */
    WorkItem item = {
        .job_id = job_id,
        .session_id = session_id,
        .recall = recall
    };
    snprintf(item.agent_name, sizeof(item.agent_name), "%s",
             agent_name ? agent_name : "Assistant");
    return pool_push(&item);
}

int llm_worker_fd(void) {
    return g_notify_pipe[0];
}

int llm_worker_read(int64_t *session_id) {
    ssize_t n = read(g_notify_pipe[0], session_id, sizeof(*session_id));
    return (n == (ssize_t)sizeof(*session_id)) ? 0 : -1;
}

void llm_worker_stop(void) {
    if (!g_started) return;
    pthread_mutex_lock(&g_pool.mtx);
    g_pool.shutdown = 1;
    pthread_cond_broadcast(&g_pool.cond);
    /* Drain barrier: block until every worker has finished its in-flight
     * request and exited. Detached threads racing process teardown (touching
     * config/db/curl state the caller is about to free) was the rare exit
     * SIGSEGV — stop() must mean stopped before the caller frees anything. */
    while (g_pool.active > 0)
        pthread_cond_wait(&g_pool.cond, &g_pool.mtx);
    pthread_mutex_unlock(&g_pool.mtx);

    /* Safe to close now: no worker can still write to notify_fd. */
    if (g_notify_pipe[0] >= 0) { close(g_notify_pipe[0]); g_notify_pipe[0] = -1; }
    if (g_notify_pipe[1] >= 0) { close(g_notify_pipe[1]); g_notify_pipe[1] = -1; }
    pthread_mutex_destroy(&g_pool.mtx);
    pthread_cond_destroy(&g_pool.cond);
    g_started = 0;
}

int llm_worker_submit_compact(sqlite3 *db, int64_t session_id, const char *agent_name) {
    if (!g_started) return -1;

    /* Persist job (crash recovery) */
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO llm_jobs(session_id, agent_name, job_type) VALUES(?,?,1)";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, session_id);
    sqlite3_bind_text(stmt, 2, agent_name ? agent_name : "Assistant", -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    int64_t job_id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;

    /* Push to thread pool */
    WorkItem item = {
        .job_id = job_id,
        .session_id = session_id,
        .recall = 0
    };
    snprintf(item.agent_name, sizeof(item.agent_name), "%s",
             agent_name ? agent_name : "Assistant");
    return pool_push(&item);
}

int llm_worker_alive(void) { return g_started; }
