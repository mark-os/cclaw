#ifndef CCLAW_LOG_H
#define CCLAW_LOG_H

#include "types.h"
#include <syslog.h>
#include <stdio.h>

/* Capture syslog priority values before we clobber the macros */
enum { CCLAW_SYSLOG_ERR = LOG_ERR, CCLAW_SYSLOG_NOTICE = LOG_NOTICE, CCLAW_SYSLOG_DBG = LOG_DEBUG };

/* V75: Logging via syslog. LOG_PERROR ensures stderr tee for CLI. */
static inline void cclaw_log_init(void) {
    openlog("cclaw", LOG_PID | LOG_PERROR, LOG_USER);
}

/* Undefine syslog macros that collide with our names */
#undef LOG_INFO
#undef LOG_DEBUG

/* Map our levels to syslog priorities */
#define CCLAW_LOG(level, cfg_ptr, fmt, ...) do { \
    if ((cfg_ptr) && (cfg_ptr)->log_level >= (level)) { \
        int _prio = (level) == LOG_LEVEL_ERROR ? CCLAW_SYSLOG_ERR : \
                    (level) == LOG_LEVEL_INFO  ? CCLAW_SYSLOG_NOTICE : CCLAW_SYSLOG_DBG; \
        syslog(_prio, fmt, ##__VA_ARGS__); \
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
