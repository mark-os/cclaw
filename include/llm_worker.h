#ifndef CCLAW_LLM_WORKER_H
#define CCLAW_LLM_WORKER_H

#include <stdint.h>
#include <sys/types.h>
#include "sqlite3.h"

/* Start in-process worker thread pool.
 * Returns 0 on success, -1 on error. */
int llm_worker_start(const char *db_path, int num_threads);

/* Submit a session for LLM processing.
 * Inserts into llm_jobs (crash recovery) and pushes to thread pool.
 * Returns 0 on success, -1 on error. */
int llm_worker_submit(sqlite3 *db, int64_t session_id, const char *agent_name, int recall);

/* Get the notification fd (for poll registration in main loop). */
int llm_worker_fd(void);

/* Read completed session_id from notification pipe. Returns 0 on success. */
int llm_worker_read(int64_t *session_id);

/* Shut down worker threads. */
void llm_worker_stop(void);

/* No-ops (kept for API compat with main.c) */
int llm_worker_respawn(void);
int llm_worker_alive(void);
pid_t llm_worker_pid(void);

#endif
