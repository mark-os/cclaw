#ifndef CCLAW_CACHE_EXPIRING_H
#define CCLAW_CACHE_EXPIRING_H

/* The CacheExpiring timer, kept in the database (plan: session hooks and
 * compaction, "CacheExpiring"; decisions 23 and 24).
 *
 * One column is the whole timer: sessions.cache_armed_at, the start of the
 * session's last request, written when its response is in and the governing
 * routing entry has a cache_lifetime. The event is due from 90% of that
 * lifetime after the request started until the lifetime is up; a session
 * whose cache has already lapsed has nothing left to save and is disarmed
 * without firing — which is also what a daemon starting after a long stop
 * does with every session it finds armed. Firing disarms, so the event fires
 * once per idle period, and the next request arms it again.
 *
 * DB only: no child, no hook, no clock of its own — `now` is the caller's,
 * so the detector is tested without sleeping. settle.c fires the hooks. */

#include <stdint.h>
#include <time.h>

#include "context.h"
#include "sqlite3.h"

/* A session the timer has just claimed for the event. */
typedef struct {
    int64_t session_id;
    char agent[64];
    int depth;          /* sessions.depth: 0 = top level */
    int lifetime;       /* the governing entry's cache_lifetime, seconds */
    time_t armed_at;    /* when its last request started */
} CacheDue;

/* A request on session_id that started at `started` has been answered: arm
 * the timer from it when the agent's governing entry has a cache_lifetime,
 * disarm it when it has none. Any thread, its own handle. 0 / -1. */
int cache_expiring_arm(sqlite3 *db, int64_t session_id, const char *agent,
                       time_t started);

/* At `now`: disarm every session whose lifetime is unknown or whose cache
 * has lapsed, and claim (disarm) up to `max` sessions that are due — idle,
 * nothing waiting in their inbox, at or past 90% of the lifetime and short
 * of 100%. A session that is due but busy stays armed and is looked at
 * again. Returns how many were claimed into `out`; -1 on a DB error. */
int cache_expiring_take(sqlite3 *db, time_t now, CacheDue *out, int max);

/* The earliest moment after `now` at which an idle armed session falls due,
 * for the event loop's poll timeout; 0 when none will. */
time_t cache_expiring_next(sqlite3 *db, time_t now);

/* The JSON object a CacheExpiring script gets as "context" (caller frees):
 * what ContextThreshold gets — tokens (the session's size now), window,
 * trim_threshold, prune_target, from `lim` — plus the session's depth (0 =
 * top level), cache_lifetime, idle_seconds (since its last answer came
 * back) and that answer's cache_read_tokens / cache_write_tokens (null when
 * the provider did not report them). */
char *cache_expiring_context(sqlite3 *db, int64_t session_id, time_t now, int tokens,
                             const ContextLimits *lim);

#endif
