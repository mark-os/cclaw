#ifndef CCLAW_LOG_H
#define CCLAW_LOG_H

#include "types.h"
#include <syslog.h>
#include <stdint.h>
#include <stdio.h>

/* Logging goes to syslog (journald captures it under `cclaw` automatically).
 * tee_stderr adds LOG_PERROR so the interactive CLI sees its own logs inline;
 * the daemon passes 0 so its logs go strictly to the system facility. */
void cclaw_log_init(int tee_stderr);

/* Per-thread logfmt context. CCLAW_LOG appends ` session=.. turn=.. agent=".."`
 * to every message while a context is set, tying log lines back to the session
 * and the llm_responses rows. The daemon advances one session per thread at a
 * time, so thread-local is exact. Pass session/turn <0 to omit that field. */
void cclaw_log_set_ctx(int64_t session_id, int64_t turn_id, const char *agent);
void cclaw_log_clear_ctx(void);

/* Format a message, append the current thread's context, and syslog() it.
 * Heap-promotes for messages larger than the stack buffer (trace dumps the
 * full response body), so nothing is truncated before syslog sees it. */
void cclaw_log_write(int prio, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Mask raw syslog() calls below the configured level (the CCLAW_LOG macros
 * filter via cfg, but bare syslog(LOG_DEBUG, ...) would still hit stderr
 * through LOG_PERROR without this). */
static inline void cclaw_log_set_level(LogLevel level) {
    int up = level >= LOG_LEVEL_DEBUG ? LOG_DEBUG :
             level == LOG_LEVEL_INFO  ? LOG_NOTICE : LOG_ERR;
    setlogmask(LOG_UPTO(up));
}

#define CCLAW_LOG(level, cfg_ptr, fmt, ...) do { \
    if ((cfg_ptr) && (cfg_ptr)->log_level >= (level)) { \
        int _prio = (level) == LOG_LEVEL_ERROR ? LOG_ERR : \
                    (level) == LOG_LEVEL_INFO  ? LOG_NOTICE : LOG_DEBUG; \
        cclaw_log_write(_prio, fmt, ##__VA_ARGS__); \
    } \
} while(0)

#define LOG_ERROR_(cfg, fmt, ...) CCLAW_LOG(LOG_LEVEL_ERROR, cfg, fmt, ##__VA_ARGS__)
#define LOG_INFO_(cfg, fmt, ...)  CCLAW_LOG(LOG_LEVEL_INFO, cfg, fmt, ##__VA_ARGS__)
#define LOG_DEBUG_(cfg, fmt, ...) CCLAW_LOG(LOG_LEVEL_DEBUG, cfg, fmt, ##__VA_ARGS__)
#define LOG_TRACE_(cfg, fmt, ...) CCLAW_LOG(LOG_LEVEL_TRACE, cfg, fmt, ##__VA_ARGS__)

/* Parse log level string → enum. Returns LOG_LEVEL_INFO on unknown. */
static inline LogLevel log_level_parse(const char *s) {
    if (!s) return LOG_LEVEL_INFO;
    if (s[0] == 't' || s[0] == 'T') return LOG_LEVEL_TRACE;
    if (s[0] == 'd' || s[0] == 'D') return LOG_LEVEL_DEBUG;
    if (s[0] == 'e' || s[0] == 'E') return LOG_LEVEL_ERROR;
    return LOG_LEVEL_INFO;
}

#endif
