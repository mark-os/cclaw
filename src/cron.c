#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "cron.h"
#include "config_registry.h"
#include "db.h"
#include "log.h"
#include "wake.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Generic pulse payload — no specific commitment. The heartbeat exists so an
 * agent is never permanently asleep; specific obligations are one-shot jobs. */
#define HEARTBEAT_PROMPT \
    "Read HEARTBEAT.md if present. Follow it. " \
    "If nothing needs attention, reply HEARTBEAT_OK."

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

int64_t cron_add(sqlite3 *db, const char *agent_name, const char *name,
                 const char *cron_expr, int64_t run_at, int64_t interval_s,
                 int64_t session_id, const char *task) {
    int64_t now = (int64_t)time(NULL);
    int has_expr     = (cron_expr && cron_expr[0]);
    int has_run_at   = (run_at > 0);
    int has_interval = (interval_s > 0);

    /* Exactly one schedule type. */
    if (has_expr + has_run_at + has_interval != 1) return -1;

    int64_t floor = config_get_int(db, "cron_min_interval_seconds");
    int64_t next;
    if (has_expr) {
        next = cron_next_run(cron_expr, now);
        if (next < 0) return -1;
        /* Floor measured fire-to-fire, not first-fire delta: cron_next_run
         * starts its search at after+60, so a first-fire delta is never a
         * meaningful rate signal (an hourly job created at :59 fires in ~60s
         * once, then hourly). Compare two consecutive fires. */
        int64_t n2 = cron_next_run(cron_expr, next);
        if (n2 < 0 || n2 - next < floor) return -1;
    } else if (has_run_at) {
        if (run_at - now < floor) return -1;
        next = run_at;
    } else { /* has_interval */
        if (interval_s < floor) return -1;
        next = now + interval_s;
    }

    const char *sql =
        "INSERT INTO cron_jobs (agent_name, name, cron_expr, run_at, interval_s,"
        "                       kind, session_id, task, next_run_at)"
        " VALUES (?,?,?,?,?,'task',?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(stmt, 1, agent_name ? agent_name : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, has_expr ? cron_expr : "", -1, SQLITE_STATIC);
    if (has_run_at) sqlite3_bind_int64(stmt, 4, run_at); else sqlite3_bind_null(stmt, 4);
    if (has_interval) sqlite3_bind_int64(stmt, 5, interval_s); else sqlite3_bind_null(stmt, 5);
    sqlite3_bind_int64(stmt, 6, session_id);
    sqlite3_bind_text(stmt, 7, task, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 8, next);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db);
}

CronJob *cron_list(sqlite3 *db, const char *agent_name, int *count) {
    *count = 0;
    const char *sql =
        "SELECT id, name, cron_expr, session_id, task, enabled, next_run_at,"
        "       last_run_at, COALESCE(run_at,0), COALESCE(interval_s,0), kind"
        " FROM cron_jobs WHERE agent_name=? ORDER BY id;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_text(stmt, 1, agent_name ? agent_name : "", -1, SQLITE_STATIC);

    int cap = 8;
    CronJob *jobs = malloc((size_t)cap * sizeof(CronJob));
    if (!jobs) { sqlite3_finalize(stmt); return NULL; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (*count >= cap) {
            cap *= 2;
            CronJob *tmp = realloc(jobs, (size_t)cap * sizeof(CronJob));
            if (!tmp) {
                cron_list_free(jobs, *count);
                *count = 0;
                sqlite3_finalize(stmt);
                return NULL;
            }
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
        j->run_at = sqlite3_column_int64(stmt, 8);
        j->interval_s = sqlite3_column_int64(stmt, 9);
        const char *k = (const char *)sqlite3_column_text(stmt, 10);
        j->kind = k ? strdup(k) : strdup("task");
        (*count)++;
    }
    sqlite3_finalize(stmt);
    if (*count == 0) { free(jobs); return NULL; }
    return jobs;
}

int cron_seed_heartbeat(sqlite3 *db, const char *agent_name) {
    if (!db || !agent_name) return -1;
    /* Idempotent: insert only if this agent has no heartbeat row yet.
     * Seeded disabled (enabled=0) — heartbeats cost an LLM call per fire, so
     * turning the pulse on stays a deliberate operator/tool act. */
    const char *sql =
        "INSERT INTO cron_jobs (agent_name, name, kind, cron_expr, interval_s,"
        "                       session_id, task, enabled, next_run_at)"
        " SELECT ?1, 'heartbeat', 'heartbeat', '', 1800, 0, '', 0, 0"
        " WHERE NOT EXISTS (SELECT 1 FROM cron_jobs"
        "                   WHERE agent_name=?1 AND kind='heartbeat');";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent_name, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 0 : -1;
}

int cron_remove(sqlite3 *db, int64_t job_id, const char *agent_name) {
    /* Agent-scoped: an agent can only delete its own jobs (review-2 F2). */
    const char *sql = "DELETE FROM cron_jobs WHERE id=? AND agent_name=?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(stmt, 1, job_id);
    sqlite3_bind_text(stmt, 2, agent_name ? agent_name : "", -1, SQLITE_STATIC);
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
        free(jobs[i].kind);
    }
    free(jobs);
}

/* Resolve an agent's most recently active session at fire time. idle_only
 * restricts to state='idle' (heartbeat targeting). Returns 0 if none. */
static int64_t recent_session(sqlite3 *db, const char *agent_name, int idle_only) {
    const char *sql = idle_only
        ? "SELECT id FROM sessions WHERE agent_name=? AND state='idle'"
          " ORDER BY updated_at DESC LIMIT 1;"
        : "SELECT id FROM sessions WHERE agent_name=?"
          " ORDER BY updated_at DESC LIMIT 1;";
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(st, 1, agent_name ? agent_name : "", -1, SQLITE_STATIC);
    int64_t sid = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : 0;
    sqlite3_finalize(st);
    return sid;
}

/* True if the session already carries an unconsumed heartbeat pulse — never
 * stack a second one on top. */
static int has_pending_heartbeat(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM inbox WHERE session_id=? AND source='heartbeat'"
            " AND consumed=0 LIMIT 1;", -1, &st, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_int64(st, 1, session_id);
    int found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

/* One due row's payload and schedule, collected before any write (an open
 * SELECT pins a WAL read snapshot; a write that needs a snapshot upgrade
 * would get an immediate SQLITE_BUSY). */
typedef struct {
    int64_t id, session_id, run_at, interval_s;
    char *expr, *task, *agent_name, *kind;
} DueJob;

/* Inject a due job into the resolved session and wake the daemon. Heartbeat
 * kind resolves to the agent's most recent idle session (skips silently if
 * none, or if a pulse is already pending); task kind resolves session_id=0 to
 * the most recent session (WARN-skips if the agent has none). */
static void fire_due(sqlite3 *db, const DueJob *j) {
    int heartbeat = (j->kind && strcmp(j->kind, "heartbeat") == 0);
    if (heartbeat) {
        int64_t sid = recent_session(db, j->agent_name, 1);
        if (sid == 0) return;                       /* busy/no idle session — no pulse needed */
        if (has_pending_heartbeat(db, sid)) return; /* never stack pulses */
        inbox_insert(db, sid, "heartbeat", HEARTBEAT_PROMPT);
        LOG_INFO_("heartbeat fire job=%lld agent=%s session=%lld",
                  (long long)j->id, j->agent_name ? j->agent_name : "", (long long)sid);
        wake_session(sid);
        return;
    }

    int64_t sid = j->session_id;
    if (sid == 0) {
        sid = recent_session(db, j->agent_name, 0);
        if (sid == 0) {
            LOG_WARN_("cron skip job=%lld agent=%s: no session to resolve",
                      (long long)j->id, j->agent_name ? j->agent_name : "");
            return;
        }
    }
    if (!j->task) return;
    inbox_insert_scanned(db, sid, "cron", j->task);
    LOG_INFO_("cron fire job=%lld agent=%s session=%lld",
              (long long)j->id, j->agent_name ? j->agent_name : "", (long long)sid);
    wake_session(sid);
}

/* Reschedule (or delete) a fired row. One-shot self-cleans; interval refires
 * every interval_s; recurring advances by cron_next_run. Replaces (not
 * follows) the old unconditional UPDATE — an empty-expr job must never fall
 * back to firing hourly forever. */
static void reschedule_due(sqlite3 *db, const DueJob *j, int64_t now) {
    if (j->run_at > 0) {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "DELETE FROM cron_jobs WHERE id=?;",
                               -1, &st, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, j->id);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        return;
    }
    int64_t next;
    if (j->interval_s > 0) {
        next = now + j->interval_s;
    } else {
        next = cron_next_run(j->expr, now);
        if (next < 0) {
            /* Unreachable for a validated recurring job; disable rather than
             * hot-loop if a malformed expr ever slips in. */
            LOG_ERROR_("cron job=%lld bad expr, disabling", (long long)j->id);
            sqlite3_stmt *st;
            if (sqlite3_prepare_v2(db, "UPDATE cron_jobs SET enabled=0 WHERE id=?;",
                                   -1, &st, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, j->id);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
            return;
        }
    }
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE cron_jobs SET last_run_at=?, next_run_at=? WHERE id=?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, now);
        sqlite3_bind_int64(st, 2, next);
        sqlite3_bind_int64(st, 3, j->id);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

static void due_free(DueJob *due, int count) {
    for (int i = 0; i < count; i++) {
        free(due[i].expr);
        free(due[i].task);
        free(due[i].agent_name);
        free(due[i].kind);
    }
    free(due);
}

/* Check and execute due cron jobs */
static void run_due_jobs(sqlite3 *db) {
    int64_t now = (int64_t)time(NULL);
    const char *sql =
        "SELECT id, cron_expr, session_id, task, agent_name, kind,"
        "       COALESCE(run_at,0), COALESCE(interval_s,0) FROM cron_jobs"
        " WHERE enabled=1 AND next_run_at <= ?;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(stmt, 1, now);

    /* Collect due jobs first (avoid holding stmt open during inbox_insert) */
    int cap = 4, count = 0;
    DueJob *due = malloc((size_t)cap * sizeof(DueJob));
    if (!due) { sqlite3_finalize(stmt); return; }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            cap *= 2;
            DueJob *tmp = realloc(due, (size_t)cap * sizeof(DueJob));
            if (!tmp) { due_free(due, count); sqlite3_finalize(stmt); return; }
            due = tmp;
        }
        due[count].id = sqlite3_column_int64(stmt, 0);
        const char *e = (const char *)sqlite3_column_text(stmt, 1);
        due[count].expr = e ? strdup(e) : NULL;
        due[count].session_id = sqlite3_column_int64(stmt, 2);
        const char *t = (const char *)sqlite3_column_text(stmt, 3);
        due[count].task = t ? strdup(t) : NULL;
        const char *a = (const char *)sqlite3_column_text(stmt, 4);
        due[count].agent_name = a ? strdup(a) : NULL;
        const char *k = (const char *)sqlite3_column_text(stmt, 5);
        due[count].kind = k ? strdup(k) : strdup("task");
        due[count].run_at = sqlite3_column_int64(stmt, 6);
        due[count].interval_s = sqlite3_column_int64(stmt, 7);
        count++;
    }
    sqlite3_finalize(stmt);

    /* Fire and reschedule each due job. Both phases only run after the reader
     * above is finalized. */
    for (int i = 0; i < count; i++) {
        fire_due(db, &due[i]);
        reschedule_due(db, &due[i], now);
    }
    due_free(due, count);
}

void cron_run_due(sqlite3 *db) {
    run_due_jobs(db);
}
