#ifndef CCLAW_LLM_WORKER_H
#define CCLAW_LLM_WORKER_H

#include <stdint.h>

/* Start LLM worker child process with thread pool.
 * Returns 0 on success, -1 on error. */
int llm_worker_start(const char *db_path, int num_threads);

/* Submit a session for LLM processing.  Writes session_id to worker pipe. */
int llm_worker_submit(int64_t session_id);

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

#endif
