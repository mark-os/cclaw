#ifndef CCLAW_LOG_H
#define CCLAW_LOG_H

#include "types.h"
#include <syslog.h>
#include <stdio.h>

/* V75: Logging via syslog. LOG_PERROR ensures stderr tee for CLI. */
static inline void cclaw_log_init(void) {
    openlog("cclaw", LOG_PID | LOG_PERROR, LOG_USER);
}

#define CCLAW_LOG(level, cfg_ptr, fmt, ...) do { \
    if ((cfg_ptr) && (cfg_ptr)->log_level >= (level)) { \
        int _prio = (level) == LOG_LEVEL_ERROR ? LOG_ERR : \
                    (level) == LOG_LEVEL_INFO  ? LOG_NOTICE : LOG_DEBUG; \
        syslog(_prio, fmt, ##__VA_ARGS__); \
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
