#ifndef CCLAW_LLM_WORKER_H
#define CCLAW_LLM_WORKER_H

#include <stdint.h>
#include <sys/types.h>
#include "sqlite3.h"

#define LLM_WORKER_QUEUE_FULL -2

/* Start LLM worker child process with thread pool.
 * Returns 0 on success, -1 on error. */
int llm_worker_start(const char *db_path, int num_threads);

/* Submit a session for LLM processing via DB job queue.
 * Returns 0 on success, -1 on error. */
int llm_worker_submit(sqlite3 *db, int64_t session_id, const char *agent_name, int recall);

/* Get the result pipe fd (for poll registration). */
int llm_worker_fd(void);

/* Read completed session_id from result pipe. Returns 0 on success. */
int llm_worker_read(int64_t *session_id);

/* Stop worker (close pipes, kill child). */
void llm_worker_stop(void);

/* Respawn worker after crash. Returns 0 on success. */
int llm_worker_respawn(void);

/* Check if worker is alive. */
int llm_worker_alive(void);

/* Get worker PID (for SIGCHLD detection). Returns -1 if not running. */
pid_t llm_worker_pid(void);

#endif
