#define _POSIX_C_SOURCE 200809L
#include "cache_expiring.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context.h"
#include "log.h"

/* The governing entry's lifetime for the session row `s` — the same entry
 * context_limits reads: the agent's first enabled chat model. */
#define SQL_LIFETIME \
    "(SELECT am.cache_lifetime FROM agent_models am JOIN models m ON m.id = am.model_id" \
    "  WHERE am.agent_name = s.agent_name AND am.purpose = 'chat'" \
    "    AND m.status != 'disabled' ORDER BY am.pos LIMIT 1)"

/* When the event falls due: 90% of the lifetime after the request started
 * (decision 24), in whole seconds. */
#define SQL_DUE "(cache_armed_at + life * 9 / 10)"

int cache_expiring_arm(sqlite3 *db, int64_t session_id, const char *agent,
                       time_t started) {
    if (!db || !agent) return -1;
    ContextLimits lim;
    context_limits(db, agent, 0, &lim);
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE sessions SET cache_armed_at = CASE WHEN ?2 > 0 THEN ?1 END"
            " WHERE id = ?3 AND cache_armed_at IS NOT CASE WHEN ?2 > 0 THEN ?1 END;",
            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)started);
    sqlite3_bind_int(st, 2, lim.lifetime);
    sqlite3_bind_int64(st, 3, session_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Disarm one session, if it is still armed from the same request: a request
 * that started meanwhile has armed it afresh, and that one stands. */
static int disarm(sqlite3 *db, int64_t session_id, time_t armed_at) {
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "UPDATE sessions SET cache_armed_at = NULL"
            " WHERE id = ?1 AND cache_armed_at = ?2;", -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)armed_at);
    int ok = sqlite3_step(st) == SQLITE_DONE && sqlite3_changes(db) == 1;
    sqlite3_finalize(st);
    return ok;
}

int cache_expiring_take(sqlite3 *db, time_t now, CacheDue *out, int max) {
    if (!db || !out || max <= 0) return -1;
    /* Read first, write only what needs it: this runs on every turn of the
     * event loop, and most of the time nothing is armed or nothing is due. */
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT id, agent_name, depth, cache_armed_at, life,"
            "       state = 'idle' AND NOT EXISTS (SELECT 1 FROM inbox i"
            "         WHERE i.session_id = a.id AND i.consumed = 0)"
            "  FROM (SELECT s.*, " SQL_LIFETIME " AS life FROM sessions s"
            "         WHERE s.cache_armed_at IS NOT NULL) a"
            " WHERE COALESCE(life, 0) <= 0"
            "    OR ?1 >= " SQL_DUE
            " ORDER BY 6 DESC, cache_armed_at LIMIT 64;", -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
    struct { CacheDue d; int idle; } rows[64];
    int n = 0;
    while (n < 64 && sqlite3_step(st) == SQLITE_ROW) {
        CacheDue *d = &rows[n].d;
        memset(d, 0, sizeof(*d));
        d->session_id = sqlite3_column_int64(st, 0);
        const char *a = (const char *)sqlite3_column_text(st, 1);
        snprintf(d->agent, sizeof(d->agent), "%s", a ? a : "");
        d->depth = sqlite3_column_int(st, 2);
        d->armed_at = (time_t)sqlite3_column_int64(st, 3);
        d->lifetime = sqlite3_column_int(st, 4);
        rows[n].idle = sqlite3_column_int(st, 5);
        n++;
    }
    sqlite3_finalize(st);

    int taken = 0;
    for (int i = 0; i < n; i++) {
        CacheDue *d = &rows[i].d;
        if (d->lifetime <= 0 || now >= d->armed_at + d->lifetime) {
            /* Unknown now, or lapsed: nothing to save. */
            if (disarm(db, d->session_id, d->armed_at))
                LOG_DEBUG_("cache expiring: session=%lld disarmed (%s)",
                           (long long)d->session_id,
                           d->lifetime <= 0 ? "no lifetime" : "cache lapsed");
            continue;
        }
        if (!rows[i].idle || taken >= max) continue;
        /* Claimed by the disarm itself: once per idle period, whatever
         * happens next. */
        if (disarm(db, d->session_id, d->armed_at)) out[taken++] = *d;
    }
    return taken;
}

time_t cache_expiring_next(sqlite3 *db, time_t now) {
    if (!db) return 0;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "SELECT MIN(" SQL_DUE ")"
            "  FROM (SELECT s.cache_armed_at, " SQL_LIFETIME " AS life FROM sessions s"
            "         WHERE s.cache_armed_at IS NOT NULL AND s.state = 'idle')"
            " WHERE life > 0 AND " SQL_DUE " > ?1;",
            -1, &st, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_int64(st, 1, (sqlite3_int64)now);
    time_t t = 0;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        t = (time_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return t;
}

char *cache_expiring_context(sqlite3 *db, int64_t session_id, time_t now, int tokens,
                             const ContextLimits *lim) {
    if (!db || !lim) return NULL;
    sqlite3_stmt *st;
    if (sqlite3_prepare_v2(db,
            "WITH r AS (SELECT cached_tokens, cache_write_tokens, created_at FROM llm_responses"
            "            WHERE session_id = ?1 AND status = 'ok' ORDER BY id DESC LIMIT 1)"
            " SELECT json_object('tokens', ?2, 'window', ?3, 'trim_threshold', ?4,"
            "  'prune_target', ?5,"
            "  'depth', (SELECT depth FROM sessions WHERE id = ?1),"
            "  'cache_lifetime', ?6,"
            "  'idle_seconds', ?7 - (SELECT created_at FROM r),"
            "  'cache_read_tokens', (SELECT cached_tokens FROM r),"
            "  'cache_write_tokens', (SELECT cache_write_tokens FROM r));",
            -1, &st, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(st, 1, session_id);
    sqlite3_bind_int(st, 2, tokens);
    sqlite3_bind_int(st, 3, lim->window);
    sqlite3_bind_int(st, 4, lim->threshold);
    sqlite3_bind_int(st, 5, lim->target);
    if (lim->lifetime > 0) sqlite3_bind_int(st, 6, lim->lifetime);
    sqlite3_bind_int64(st, 7, (sqlite3_int64)now);
    char *out = NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(st, 0);
        if (t) out = strdup(t);
    }
    sqlite3_finalize(st);
    return out;
}
