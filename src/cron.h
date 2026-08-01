#ifndef CCLAW_CRON_H
#define CCLAW_CRON_H

/* Cron schedule parsing, next-run computation, and the cron_jobs writer.
 * A job is described by a *document* (the cron_set tool's JSON arguments):
 * every field optional, validity in cross-field rules, same-name = upsert.
 * The same document is what a cross-agent approval parks and what the apply
 * path replays, so there is exactly one representation of a requested job.
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

/* One cron_set document, parsed. The has_* flags are what an upsert merges
 * on: absent keeps the stored value, and "" on prompt/script clears it. */
typedef struct {
    char *name;
    char *cron_expr;
    int64_t in_seconds;
    char *prompt;
    char *script;
    int64_t session_id;
    char *session;        /* only legal value: "new" */
    char *agent;          /* target agent, session:"new" only */
    char *channel_name;
    char *chat_id;
    int has_expr, has_seconds, has_prompt, has_script;
    int has_session, has_session_id, has_agent, has_channel, has_chat;
} CronDoc;

/* Parse a document. Returns 0 (fields owned by *out), -1 on malformed JSON. */
int cron_doc_parse(sqlite3 *db, const char *doc, CronDoc *out);
void cron_doc_free(CronDoc *d);

/* Schedule verdicts, kept distinct so the caller can write the right message:
 * an unparseable expression and a too-frequent one are different mistakes. */
typedef enum {
    CRON_SCHED_OK = 0,
    CRON_SCHED_NONE,      /* neither cron_expr nor in_seconds given */
    CRON_SCHED_BOTH,      /* both given */
    CRON_SCHED_BAD_EXPR,  /* cron_expr does not parse */
    CRON_SCHED_FLOOR      /* fires more often than cron_min_interval_seconds */
} CronSchedRc;

/* Check one schedule spec (exactly one of cron_expr / in_seconds). *next_out
 * gets the first fire time, *every_out the fire-to-fire spacing measured
 * against the floor — for cron_expr that is two consecutive fires, never the
 * first-fire delta (an hourly job created at :59 fires once in ~60s). Both
 * out-params are optional; *every_out is set even on CRON_SCHED_FLOOR so the
 * refusal can echo the offending value. */
CronSchedRc cron_schedule_check(sqlite3 *db, const char *cron_expr,
                                int64_t in_seconds, int64_t *next_out,
                                int64_t *every_out);

/* Validate a document against the MERGED row (document ⊕ stored job) without
 * writing: the merged job must have a schedule and one unambiguous target
 * mode. Returns 0, or -1 with *err_out set to a heap message naming the rule
 * (caller frees). Per-call rules the tool can phrase better (bad expression,
 * floor, script path, channel authority) are checked by the tool. */
int cron_doc_check(sqlite3 *db, const char *agent_name, const char *doc,
                   char **err_out);

/* Upsert a job by (agent_name, name) from a document: fields the document
 * provides replace, the rest keep their stored value. caller_session_id is
 * the session that asked (the follow-the-conversation stamp and the fire
 * anchor); 0 for operator/manifest callers. *updated_out is 1 when an
 * existing job was replaced, 0 when one was created. Returns the job id, or
 * -1 with *err_out set (both out-params optional). */
int64_t cron_upsert(sqlite3 *db, const char *agent_name,
                    int64_t caller_session_id, const char *doc,
                    int *updated_out, char **err_out);

/* Execute any due cron jobs now. Called from the main event loop (daemon). */
void cron_run_due(sqlite3 *db);

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
