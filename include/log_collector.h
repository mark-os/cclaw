#ifndef CCLAW_LOG_COLLECTOR_H
#define CCLAW_LOG_COLLECTOR_H

#include <sys/types.h>
#include <stdint.h>

/* T218/V75: Log collector process.
 * Daemon spawns collector at startup. Collector receives fds via SCM_RIGHTS
 * over unix socketpair, epoll-monitors them, batch-inserts lines to journal.db.
 *
 * API (daemon side):
 *   log_collector_start(journal_path) — fork collector, returns 0 on success
 *   log_collector_send_fd(fd, source, pid, session_id, stream) — pass fd to collector
 *   log_collector_stop() — signal collector to exit, waitpid
 */

/* Start log collector process. Returns 0 on success, -1 on error.
 * journal_path: path to journal.db file. */
int log_collector_start(const char *journal_path);

/* Send an fd to the collector via SCM_RIGHTS with metadata.
 * stream: 1=stdout, 2=stderr. Returns 0 on success. */
int log_collector_send_fd(int fd, const char *source, pid_t pid,
                          int64_t session_id, int stream);

/* Stop collector (SIGTERM + waitpid). */
void log_collector_stop(void);

/* Get collector pid (for cleanup on daemon exit). Returns 0 if not running. */
pid_t log_collector_pid(void);

#endif
