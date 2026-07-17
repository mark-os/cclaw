#ifndef CCLAW_CRON_H
#define CCLAW_CRON_H

/* Cron schedule parsing and next-run computation — pure time math over the
 * cron_jobs table's schedule columns (cron_expr / run_at / interval_s). No
 * I/O; the daemon loop calls cron_next_run to decide when to fire.
 */

#include <stdint.h>
#include "sqlite3.h"

/* Cron job record. Schedule is exactly one of cron_expr (recurring), run_at
 * (one-shot), interval_s (fixed period). kind: "task" | "heartbeat". */
typedef struct {
    int64_t id;
    char *name;
    char *cron_expr;    /* "M H D Mo DoW" — standard 5-field cron; "" if unused */
    int64_t run_at;     /* one-shot fire time (unix ts); 0 if unused */
    int64_t interval_s; /* fixed period in seconds; 0 if unused */
    char *kind;         /* "task" | "heartbeat" */
    int64_t session_id;
    char *task;         /* user message to inject */
    int enabled;
    int64_t next_run_at;
    int64_t last_run_at;
} CronJob;

/* Execute any due cron jobs now. Called from the main event loop (daemon). */
void cron_run_due(sqlite3 *db);

/* CRUD — daemon-only management. Schedule is exactly one of cron_expr
 * (non-empty), run_at (>0), or interval_s (>0); anything else returns -1.
 * A min-interval floor (config cron_min_interval_seconds, default 300) is
 * enforced here on all three schedule types. Always creates kind='task'. */
int64_t cron_add(sqlite3 *db, const char *agent_name, const char *name,
                 const char *cron_expr, int64_t run_at, int64_t interval_s,
                 int64_t session_id, const char *task);
CronJob *cron_list(sqlite3 *db, const char *agent_name, int *count);
/* Agent-scoped delete: only removes the job if agent_name owns it. */
int cron_remove(sqlite3 *db, int64_t job_id, const char *agent_name);
void cron_list_free(CronJob *jobs, int count);

/* Seed a disabled heartbeat pulse row (kind='heartbeat', 1800s cadence) for
 * an agent, idempotent — a no-op if the agent already has one. Called at
 * agent creation so every agent has an inspectable, enable-able pulse.
 * Returns 0 on success (including the already-present no-op), -1 on DB error. */
int cron_seed_heartbeat(sqlite3 *db, const char *agent_name);

/* Parse 5-field cron expression, compute next run time after `after`.
 * Returns unix timestamp or -1 on parse error. */
int64_t cron_next_run(const char *cron_expr, int64_t after);

#endif
