#ifndef CCLAW_LOG_H
#define CCLAW_LOG_H

#include "types.h"
#include <stdio.h>
#include <time.h>

/* V97: Logging macros. All output goes to stderr.
 * Timestamp + level prefix for journal.db parsing.
 * Usage: LOG_INFO(cfg, "turn %d started", turn_id); */

#define CCLAW_LOG(level, cfg_ptr, fmt, ...) do { \
    if ((cfg_ptr) && (cfg_ptr)->log_level >= (level)) { \
        struct timespec _ts; \
        clock_gettime(CLOCK_REALTIME, &_ts); \
        struct tm _tm; \
        localtime_r(&_ts.tv_sec, &_tm); \
        fprintf(stderr, "%02d:%02d:%02d.%03ld [%s] " fmt "\n", \
                _tm.tm_hour, _tm.tm_min, _tm.tm_sec, \
                _ts.tv_nsec / 1000000, \
                (level) == LOG_LEVEL_ERROR ? "ERROR" : \
                (level) == LOG_LEVEL_INFO  ? "INFO"  : \
                (level) == LOG_LEVEL_DEBUG ? "DEBUG" : "TRACE", \
                ##__VA_ARGS__); \
    } \
} while(0)

#define LOG_ERROR(cfg, fmt, ...) CCLAW_LOG(LOG_LEVEL_ERROR, cfg, fmt, ##__VA_ARGS__)
#define LOG_INFO(cfg, fmt, ...)  CCLAW_LOG(LOG_LEVEL_INFO, cfg, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(cfg, fmt, ...) CCLAW_LOG(LOG_LEVEL_DEBUG, cfg, fmt, ##__VA_ARGS__)
#define LOG_TRACE(cfg, fmt, ...) CCLAW_LOG(LOG_LEVEL_TRACE, cfg, fmt, ##__VA_ARGS__)

/* Parse log level string → enum. Returns LOG_LEVEL_INFO on unknown. */
static inline LogLevel log_level_parse(const char *s) {
    if (!s) return LOG_LEVEL_INFO;
    if (s[0] == 't' || s[0] == 'T') return LOG_LEVEL_TRACE;
    if (s[0] == 'd' || s[0] == 'D') return LOG_LEVEL_DEBUG;
    if (s[0] == 'e' || s[0] == 'E') return LOG_LEVEL_ERROR;
    return LOG_LEVEL_INFO;
}

#endif
