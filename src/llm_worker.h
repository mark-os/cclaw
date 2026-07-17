#ifndef CCLAW_LLM_WORKER_H
#define CCLAW_LLM_WORKER_H

/* The in-process LLM worker thread pool: blocking curl calls run here (not
 * on the event loop), writing results back to the DB and waking the poll
 * loop. Reuses threads to amortize the TLS handshake.
 */

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

/* Submit a session for compaction (summarization).
 * Returns 0 on success, -1 on error. */
int llm_worker_submit_compact(sqlite3 *db, int64_t session_id, const char *agent_name);

/* Submit a media_jobs row for transcription. The media_jobs row itself is the
 * crash-recovery record (no llm_jobs row). Returns 0 on success, -1 on error. */
int llm_worker_submit_transcribe(sqlite3 *db, int64_t media_job_id,
                                 int64_t session_id, const char *agent_name);

/* Resubmit media_jobs rows left over from a crashed run (daemon start).
 * Returns the number of jobs resubmitted. */
int llm_worker_resubmit_media(sqlite3 *db);

/* Check if worker pool is running */
int llm_worker_alive(void);

#endif
