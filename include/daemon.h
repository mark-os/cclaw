#ifndef CCLAW_DAEMON_H
#define CCLAW_DAEMON_H

#include "types.h"
#include "sqlite3.h"
#include <sys/types.h>
#include <stdint.h>

/* Max concurrent agent processes system-wide */
#define DAEMON_MAX_CHILDREN 32

/* T82/V25: Signal pipe — inserters write session_id to wake daemon.
 * Returns 0 on success, -1 on error. */
int daemon_signal_init(void);

/* Write session_id to signal pipe (non-blocking, safe from any thread). */
int daemon_signal_session(int64_t session_id);

/* Signal daemon from external process via named FIFO. Returns 0 on success. */
int daemon_signal_external(const char *db_path, int64_t session_id);

/* Close signal pipe (cleanup). */
void daemon_signal_close(void);

/* Get read-end fd for epoll registration. */
int daemon_signal_fd(void);

/* T81: Run daemon main loop (blocks until shutdown).
 * Epoll on signal_pipe + SIGCHLD self-pipe, fork/reap agents. */
int daemon_run(const Config *cfg, sqlite3 *db);

/* T86/V27: Update last_route for session from newest inbox source.
 * Called by agent process after inbox consumption. */
int session_set_last_route(sqlite3 *db, int64_t session_id, const char *route);

/* Read last_route for session. Caller frees. Returns NULL if unset. */
char *session_get_last_route(sqlite3 *db, int64_t session_id);

/* T87: Get session_id for a tracked child pid. Returns -1 if not found. */
int64_t daemon_child_session(pid_t pid);

/* T94/V34: Startup recovery — reset non-idle sessions, handle orphans. */
void daemon_startup_recovery(sqlite3 *db);

/* Override exec path for fork+exec (testing). */
void daemon_set_self_path(const char *path);

/* T189/V68: Bootstrap — if no named agents exist, create ephemeral bootstrap agent.
 * Returns session_id of bootstrap session (>0) or 0 if no bootstrap needed, -1 on error. */
int64_t daemon_bootstrap(sqlite3 *db);

/* Derive FIFO path from db_path (caller frees). */
char *daemon_pipe_path(const char *db_path);

/* Check if daemon is running by probing the named FIFO.
 * Returns 1 if daemon alive, 0 if not, -1 on error. */
int daemon_is_running(const char *db_path);

/* Create named FIFO and open read end. Returns fd or -1. */
int daemon_fifo_open(const char *db_path);

/* Clean up named FIFO on daemon exit. */
void daemon_fifo_close(int fd, const char *db_path);

/* V71/T194: Token rate limiting — rolling 1h window, in-memory only. */
void daemon_token_usage_add(int tokens);
int daemon_token_usage_hourly(void);
void daemon_token_usage_reset(void);

#endif
