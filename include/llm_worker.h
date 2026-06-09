#ifndef CCLAW_LLM_WORKER_H
#define CCLAW_LLM_WORKER_H

#include <stdint.h>
#include <sys/types.h>

#define LLM_WORKER_QUEUE_FULL -2

/* Start LLM worker child process with thread pool.
 * Returns 0 on success, -1 on error. */
int llm_worker_start(const char *db_path, int num_threads);

/* Submit a session for LLM processing.
 * Returns 0 on success, -1 on pipe error, LLM_WORKER_QUEUE_FULL on overflow. */
int llm_worker_submit(int64_t session_id, int recall);

/* Get the result pipe fd (for epoll registration). */
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
