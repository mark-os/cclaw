#ifndef CCLAW_LOG_H
#define CCLAW_LOG_H

#include "types.h"
#include <stdint.h>

/* Logging goes to syslog (journald captures it under `cclaw` automatically).
 * tee_stderr adds LOG_PERROR so the interactive CLI sees its own logs inline;
 * the daemon passes 0 so its logs go strictly to the system facility. */
void cclaw_log_init(int tee_stderr);

/* Per-thread logfmt context. cclaw_log_write appends ` session=.. turn=.. agent=".."`
 * to every message while a context is set, tying log lines back to the session
 * and the llm_responses rows. The daemon advances one session per thread at a
 * time, so thread-local is exact. Pass session/turn <0 to omit that field. */
void cclaw_log_set_ctx(int64_t session_id, int64_t turn_id, const char *agent);
void cclaw_log_clear_ctx(void);

/* Single source of truth for the process log level. Stores the level that
 * cclaw_log_write gates on, and also masks raw syslog() calls from vendored
 * code below the same level (belt and suspenders). */
void cclaw_log_set_level(LogLevel level);
LogLevel cclaw_log_level(void);

/* Gate on the process level, format, append the current thread's context, and
 * syslog() it. Returns before formatting when gated, so trace-size bodies cost
 * nothing at lower levels. Heap-promotes for messages larger than the stack
 * buffer (trace dumps the full response body), so nothing is truncated before
 * syslog sees it. TRACE lines carry a ` trace=1` suffix so journalctl can
 * separate them from DEBUG (both map to syslog LOG_DEBUG). */
void cclaw_log_write(LogLevel level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#define LOG_ERROR_(fmt, ...) cclaw_log_write(LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define LOG_WARN_(fmt, ...)  cclaw_log_write(LOG_LEVEL_WARN,  fmt, ##__VA_ARGS__)
#define LOG_INFO_(fmt, ...)  cclaw_log_write(LOG_LEVEL_INFO,  fmt, ##__VA_ARGS__)
#define LOG_DEBUG_(fmt, ...) cclaw_log_write(LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#define LOG_TRACE_(fmt, ...) cclaw_log_write(LOG_LEVEL_TRACE, fmt, ##__VA_ARGS__)

/* Level → canonical name (config strings, CCLAW_LOG_LEVEL propagation). */
static inline const char *log_level_name(LogLevel l) {
    return l >= LOG_LEVEL_TRACE ? "trace" :
           l == LOG_LEVEL_DEBUG ? "debug" :
           l == LOG_LEVEL_WARN  ? "warn"  :
           l == LOG_LEVEL_ERROR ? "error" : "info";
}

/* Parse log level string → enum. Returns LOG_LEVEL_INFO on unknown. */
static inline LogLevel log_level_parse(const char *s) {
    if (!s) return LOG_LEVEL_INFO;
    if (s[0] == 't' || s[0] == 'T') return LOG_LEVEL_TRACE;
    if (s[0] == 'd' || s[0] == 'D') return LOG_LEVEL_DEBUG;
    if (s[0] == 'w' || s[0] == 'W') return LOG_LEVEL_WARN;
    if (s[0] == 'e' || s[0] == 'E') return LOG_LEVEL_ERROR;
    return LOG_LEVEL_INFO;
}

#endif
