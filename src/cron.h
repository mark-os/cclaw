#ifndef CCLAW_CRON_H
#define CCLAW_CRON_H

#include <stdint.h>
#include "sqlite3.h"

/* Cron job record */
typedef struct {
    int64_t id;
    char *name;
    char *cron_expr;    /* "M H D Mo DoW" — standard 5-field cron */
    int64_t session_id;
    char *task;         /* user message to inject */
    int enabled;
    int64_t next_run_at;
    int64_t last_run_at;
} CronJob;

/* Execute any due cron jobs now. Called from the main event loop (daemon). */
void cron_run_due(sqlite3 *db);

/* CRUD — daemon-only management (T204, V76) */
int64_t cron_add(sqlite3 *db, const char *agent_name, const char *name,
                 const char *cron_expr, int64_t session_id, const char *task);
CronJob *cron_list(sqlite3 *db, const char *agent_name, int *count);
int cron_remove(sqlite3 *db, int64_t job_id);
void cron_list_free(CronJob *jobs, int count);

/* Parse 5-field cron expression, compute next run time after `after`.
 * Returns unix timestamp or -1 on parse error. */
int64_t cron_next_run(const char *cron_expr, int64_t after);

#endif
