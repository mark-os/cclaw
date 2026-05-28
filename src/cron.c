#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "cron.h"
#include "db.h"
#include "daemon.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_t cron_thread;
static volatile int cron_running;
static const Config *cron_cfg;
static sqlite3 *cron_db;

/* Parse a single cron field. Supports: star, N, N-M, star/N, N-M/S, comma-separated.
 * Sets bits in out for values in [min, max]. Returns 0 on success. */
static int parse_field(const char *field, int min, int max, uint64_t *out) {
    *out = 0;
    const char *p = field;
    while (*p) {
        int start = min, end = max, step = 1;
        if (*p == '*') {
            p++;
            start = min; end = max;
        } else {
            start = (int)strtol(p, (char **)&p, 10);
            if (start < min || start > max) return -1;
            if (*p == '-') {
                p++;
                end = (int)strtol(p, (char **)&p, 10);
                if (end < min || end > max) return -1;
            } else {
                end = start;
            }
        }
        if (*p == '/') {
            p++;
            step = (int)strtol(p, (char **)&p, 10);
            if (step <= 0) return -1;
        }
        for (int i = start; i <= end; i += step)
            *out |= (1ULL << i);
        if (*p == ',') p++;
        else if (*p != '\0') return -1;
    }
    return 0;
}

int64_t cron_next_run(const char *cron_expr, int64_t after) {
    if (!cron_expr) return -1;

    /* Parse 5 fields: min hour dom month dow */
    char buf[128];
    strncpy(buf, cron_expr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *fields[5];
    char *saveptr = NULL;
    char *tok = strtok_r(buf, " \t", &saveptr);
    for (int i = 0; i < 5; i++) {
        if (!tok) return -1;
        fields[i] = tok;
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    uint64_t minutes, hours, doms, months, dows;
    if (parse_field(fields[0], 0, 59, &minutes) != 0) return -1;
    if (parse_field(fields[1], 0, 23, &hours) != 0) return -1;
    if (parse_field(fields[2], 1, 31, &doms) != 0) return -1;
    if (parse_field(fields[3], 1, 12, &months) != 0) return -1;
    if (parse_field(fields[4], 0, 6, &dows) != 0) return -1;

    /* Start from after+60, rounded to minute boundary */
    time_t t = (time_t)(after + 60);
    t -= t % 60;

    /* Search up to 366 days ahead */
    time_t limit = t + 366 * 24 * 3600;
    struct tm tm;

    while (t < limit) {
        gmtime_r(&t, &tm);
        if (!(months & (1ULL << (tm.tm_mon + 1)))) {
            tm.tm_mon++;
            tm.tm_mday = 1;
            tm.tm_hour = 0;
            tm.tm_min = 0;
            t = timegm(&tm);
            continue;
        }
        if (!(doms & (1ULL << tm.tm_mday)) || !(dows & (1ULL << tm.tm_wday))) {
            t += 86400 - (tm.tm_hour * 3600 + tm.tm_min * 60);
            continue;
        }
        if (!(hours & (1ULL << tm.tm_hour))) {
            t += 3600 - (tm.tm_min * 60);
            continue;
        }
        if (!(minutes & (1ULL << tm.tm_min))) {
            t += 60;
            continue;
        }
        return (int64_t)t;
    }
    return -1;
}

int64_t cron_add(sqlite3 *db, const char *name, const char *cron_expr,
                 int64_t session_id, const char *task) {
    int64_t now = (int64_t)time(NULL);
    int64_t next = cron_next_run(cron_expr, now);
    if (next < 0) return -1;

    const char *sql =
        "INSERT INTO cron_jobs (name, cron_expr, session_id, task, next_run_at)"
        " VALUES (?,?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cron_expr, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 3, session_id);
    sqlite3_bind_text(stmt, 4, task, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, next);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db);
}

CronJob *cron_list(sqlite3 *db, int64_t session_id, int *count) {
    *count = 0;
    const char *sql =
        "SELECT id, name, cron_expr, session_id, task, enabled, next_run_at, last_run_at"
        " FROM cron_jobs WHERE session_id=? ORDER BY id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(stmt, 1, session_id);

    int cap = 8;
    CronJob *jobs = malloc((size_t)cap * sizeof(CronJob));
    if (!jobs) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            CronJob *tmp = realloc(jobs, (size_t)cap * sizeof(CronJob));
            if (!tmp) break;
            jobs = tmp;
        }
        CronJob *j = &jobs[*count];
        j->id = sqlite3_column_int64(stmt, 0);
        const char *n = (const char *)sqlite3_column_text(stmt, 1);
        j->name = n ? strdup(n) : NULL;
        const char *ce = (const char *)sqlite3_column_text(stmt, 2);
        j->cron_expr = ce ? strdup(ce) : NULL;
        j->session_id = sqlite3_column_int64(stmt, 3);
        const char *t = (const char *)sqlite3_column_text(stmt, 4);
        j->task = t ? strdup(t) : NULL;
        j->enabled = sqlite3_column_int(stmt, 5);
        j->next_run_at = sqlite3_column_int64(stmt, 6);
        j->last_run_at = sqlite3_column_int64(stmt, 7);
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(jobs); return NULL; }
    return jobs;
}

int cron_remove(sqlite3 *db, int64_t job_id) {
    const char *sql = "DELETE FROM cron_jobs WHERE id=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, job_id);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE && sqlite3_changes(db) > 0) ? 0 : -1;
}

void cron_list_free(CronJob *jobs, int count) {
    if (!jobs) return;
    for (int i = 0; i < count; i++) {
        free(jobs[i].name);
        free(jobs[i].cron_expr);
        free(jobs[i].task);
    }
    free(jobs);
}

/* Insert cron task into inbox and signal daemon to process */
static void execute_job(int64_t session_id, const char *task) {
    char *aname = session_get_agent_name(cron_db, session_id);
    if (aname) {
        daemon_inbox_insert(aname, session_id, "cron", task);
        free(aname);
    } else {
        inbox_insert(cron_db, session_id, "cron", task);
    }
    daemon_signal_session(session_id);
}

/* Check and execute due cron jobs */
static void run_due_jobs(void) {
    int64_t now = (int64_t)time(NULL);
    const char *sql =
        "SELECT id, cron_expr, session_id, task FROM cron_jobs"
        " WHERE enabled=1 AND next_run_at <= ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(cron_db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, now);

    /* Collect due jobs first (avoid holding stmt open during agent_run) */
    typedef struct { int64_t id; char *expr; int64_t session_id; char *task; } DueJob;
    int cap = 4, count = 0;
    DueJob *due = malloc((size_t)cap * sizeof(DueJob));
    if (!due) { sqlite3_finalize(stmt); return; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            DueJob *tmp = realloc(due, (size_t)cap * sizeof(DueJob));
            if (!tmp) break;
            due = tmp;
        }
        due[count].id = sqlite3_column_int64(stmt, 0);
        const char *e = (const char *)sqlite3_column_text(stmt, 1);
        due[count].expr = e ? strdup(e) : NULL;
        due[count].session_id = sqlite3_column_int64(stmt, 2);
        const char *t = (const char *)sqlite3_column_text(stmt, 3);
        due[count].task = t ? strdup(t) : NULL;
        count++;
    }
    sqlite3_finalize(stmt);

    /* Execute each due job */
    for (int i = 0; i < count; i++) {
        if (due[i].task)
            execute_job(due[i].session_id, due[i].task);

        /* Update last_run_at and next_run_at */
        int64_t next = cron_next_run(due[i].expr, now);
        const char *upd =
            "UPDATE cron_jobs SET last_run_at=?, next_run_at=? WHERE id=?;";
        sqlite3_stmt *ustmt;
        if (sqlite3_prepare_v2(cron_db, upd, -1, &ustmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ustmt, 1, now);
            sqlite3_bind_int64(ustmt, 2, next > 0 ? next : now + 3600);
            sqlite3_bind_int64(ustmt, 3, due[i].id);
            sqlite3_step(ustmt);
            sqlite3_finalize(ustmt);
        }
        free(due[i].expr);
        free(due[i].task);
    }
    free(due);
}

static void *cron_loop(void *arg) {
    (void)arg;
    while (cron_running) {
        /* Sleep 60s in 1s increments for responsive shutdown */
        for (int i = 0; i < 60 && cron_running; i++)
            sleep(1);
        if (cron_running)
            run_due_jobs();
    }
    return NULL;
}

int cron_start(const Config *cfg, sqlite3 *db) {
    if (!cfg || !db) return -1;

    cron_cfg = cfg;
    cron_db = db;
    cron_running = 1;

    if (pthread_create(&cron_thread, NULL, cron_loop, NULL) != 0) {
        cron_running = 0;
        return -1;
    }
    return 0;
}

void cron_stop(void) {
    if (!cron_running) return;
    cron_running = 0;
    pthread_join(cron_thread, NULL);
}
