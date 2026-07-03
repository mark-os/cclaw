#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static LogLevel g_log_level = LOG_LEVEL_INFO;

void cclaw_log_init(int tee_stderr) {
    int opts = LOG_PID | (tee_stderr ? LOG_PERROR : 0);
    openlog("cclaw", opts, LOG_USER);
}

void cclaw_log_set_level(LogLevel level) {
    g_log_level = level;
    /* Also mask raw syslog() from vendored code below the same level —
     * cclaw_log_write gates itself, but a bare syslog(LOG_DEBUG, ...) would
     * still hit stderr through LOG_PERROR without this. */
    int up = level >= LOG_LEVEL_DEBUG ? LOG_DEBUG :
             level == LOG_LEVEL_INFO  ? LOG_NOTICE :
             level == LOG_LEVEL_WARN  ? LOG_WARNING : LOG_ERR;
    setlogmask(LOG_UPTO(up));
}

LogLevel cclaw_log_level(void) {
    return g_log_level;
}

/* Thread-local logfmt context. Shared across translation units via this one
 * definition; a `static __thread` in the header would give each TU its own. */
static __thread int64_t g_ctx_session = -1;
static __thread int64_t g_ctx_turn    = -1;
static __thread char    g_ctx_agent[64];

void cclaw_log_set_ctx(int64_t session_id, int64_t turn_id, const char *agent) {
    g_ctx_session = session_id;
    g_ctx_turn    = turn_id;
    if (agent) snprintf(g_ctx_agent, sizeof g_ctx_agent, "%s", agent);
    else       g_ctx_agent[0] = '\0';
}

void cclaw_log_clear_ctx(void) {
    g_ctx_session = -1;
    g_ctx_turn    = -1;
    g_ctx_agent[0] = '\0';
}

/* Render the active context as a logfmt suffix (leading space, or empty). */
static void ctx_suffix(char *out, size_t cap) {
    size_t o = 0;
    out[0] = '\0';
    if (g_ctx_session >= 0)
        o += (size_t)snprintf(out + o, cap - o, " session=%lld", (long long)g_ctx_session);
    if (g_ctx_turn >= 0 && o < cap)
        o += (size_t)snprintf(out + o, cap - o, " turn=%lld", (long long)g_ctx_turn);
    if (g_ctx_agent[0] && o < cap)
        snprintf(out + o, cap - o, " agent=\"%s\"", g_ctx_agent);
    out[cap - 1] = '\0';
}

void cclaw_log_write(LogLevel level, const char *fmt, ...) {
    if (level > g_log_level) return;

    char stackbuf[1024];
    char *msg = stackbuf;
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(stackbuf, sizeof stackbuf, fmt, ap);
    va_end(ap);

    /* Promote to the heap rather than truncate (trace logs the full response
     * body, and glibc's syslog() handles arbitrary-length messages). */
    if (n >= (int)sizeof stackbuf) {
        char *heap = malloc((size_t)n + 1);
        if (heap) {
            va_start(ap, fmt);
            vsnprintf(heap, (size_t)n + 1, fmt, ap);
            va_end(ap);
            msg = heap;
        }
    }

    char suffix[160];
    ctx_suffix(suffix, sizeof suffix);
    int prio = level == LOG_LEVEL_ERROR ? LOG_ERR :
               level == LOG_LEVEL_WARN  ? LOG_WARNING :
               level == LOG_LEVEL_INFO  ? LOG_NOTICE : LOG_DEBUG;
    syslog(prio, "%s%s%s", msg, suffix,
           level == LOG_LEVEL_TRACE ? " trace=1" : "");

    if (msg != stackbuf) free(msg);
}
