#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "channel.h"
#include "approval.h"
#include "log.h"
#include "db.h"
#include "secret_scan.h"
#include "wake.h"
#include "resolve.h"
#include "config_registry.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/wait.h>
#include <unistd.h>

static ChannelProc g_channels[CHANNEL_MAX];
static int g_count;

static ChannelProc *find_by_pid(pid_t pid) {
    for (int i = 0; i < g_count; i++)
        if (g_channels[i].pid == pid) return &g_channels[i];
    return NULL;
}

static void remove_channel(ChannelProc *c) {
    *c = g_channels[--g_count];
}

static pid_t do_fork(const char *name) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Re-exec self for a clean process image; the --channel branch runs the
         * channel loop in-process (channel_runner_main), so ps shows
         * `cclaw --channel <name>`. */
        execl("/proc/self/exe", "cclaw", "--channel", name, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static void update_pid(sqlite3 *db, const char *name, pid_t pid) {
    const char *sql = "UPDATE channels SET pid=? WHERE name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_int(s, 1, (int)pid);
        sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(s); sqlite3_finalize(s);
    }
}


int channel_mark_validated(sqlite3 *db, const char *name) {
    const char *sql = "UPDATE channels SET status='validated'"
                      " WHERE name=? AND status IN ('draft','broken');";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    return changed > 0 ? 0 : -1;
}

int channel_activate(sqlite3 *db, const char *name) {
    const char *sql = "UPDATE channels SET status='active'"
                      " WHERE name=? AND status='validated';";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_step(s);
    int changed = sqlite3_changes(db);
    sqlite3_finalize(s);
    return changed > 0 ? 0 : -1;
}

char *channel_get_status(sqlite3 *db, const char *name) {
    const char *sql = "SELECT status FROM channels WHERE name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    char *status = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(s, 0);
        if (v) status = strdup(v);
    }
    sqlite3_finalize(s);
    return status;
}

int channel_launch_all(sqlite3 *db) {
    /* Collect names first, finalize, then fork + update_pid: writing (or
     * forking) with the SELECT still open pins a read snapshot, and the
     * update_pid write would fail with an immediate SQLITE_BUSY if any other
     * process committed meanwhile (snapshot upgrades skip busy_timeout). */
    const char *sql = "SELECT c.name FROM channels c"
                      " JOIN extensions e ON c.extension_name=e.name"
                      " WHERE c.status='active';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    char names[CHANNEL_MAX][64];  /* matches ChannelProc.name */
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && g_count + n < CHANNEL_MAX) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        if (!name) continue;
        snprintf(names[n], sizeof(names[n]), "%s", name);
        n++;
    }
    sqlite3_finalize(stmt);

    int launched = 0;
    for (int i = 0; i < n; i++) {
        pid_t pid = do_fork(names[i]);
        if (pid > 0) {
            ChannelProc *c = &g_channels[g_count++];
            c->pid = pid;
            c->restart_count = 0;
            c->first_crash = 0;
            c->next_restart_at = 0;
            c->started_at = time(NULL);
            snprintf(c->name, sizeof(c->name), "%s", names[i]);
            update_pid(db, names[i], pid);
            launched++;
        }
    }
    return launched;
}


void channel_shutdown_all(void) {
    for (int i = 0; i < g_count; i++)
        if (g_channels[i].pid > 0) kill(g_channels[i].pid, SIGTERM);
    for (int attempt = 0; attempt < 10 && g_count > 0; attempt++) {
        usleep(100000);
        int status; pid_t p;
        while ((p = waitpid(-1, &status, WNOHANG)) > 0) {
            ChannelProc *c = find_by_pid(p);
            if (c) remove_channel(c);
        }
    }
    for (int i = 0; i < g_count; i++)
        if (g_channels[i].pid > 0) kill(g_channels[i].pid, SIGKILL);
    g_count = 0;
}
int channel_reap(pid_t pid, sqlite3 *db) {
    ChannelProc *c = find_by_pid(pid);
    if (!c) return 0;

    time_t now = time(NULL);
    c->pid = -1;
    c->restart_count++;

    if (c->first_crash == 0) c->first_crash = now;

    /* Flap detection: 3+ crashes in 5 minutes → broken */
    if (c->restart_count >= CHANNEL_MAX_RESTARTS &&
        (now - c->first_crash) < CHANNEL_FLAP_WINDOW) {
        LOG_ERROR_("channel flapping name=%s crashes=%d window=%lds status=broken",
                   c->name, c->restart_count, (long)(now - c->first_crash));
        const char *sql = "UPDATE channels SET status='broken' WHERE name=?;";
        sqlite3_stmt *s;
        if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
            sqlite3_bind_text(s, 1, c->name, -1, SQLITE_STATIC);
            sqlite3_step(s); sqlite3_finalize(s);
        }
        remove_channel(c);
        return 1;
    }

    /* Exponential backoff: min(60, 1 << restart_count) */
    int delay = 1 << c->restart_count;
    if (delay > CHANNEL_MAX_BACKOFF) delay = CHANNEL_MAX_BACKOFF;
    c->next_restart_at = now + delay;
    LOG_INFO_("channel died name=%s pid=%d restart_count=%d delay=%ds",
              c->name, (int)pid, c->restart_count, delay);
    return 1;
}

void channel_tick(sqlite3 *db) {
    time_t now = time(NULL);
    for (int i = 0; i < g_count; i++) {
        ChannelProc *c = &g_channels[i];

        /* Restart channels whose backoff expired */
        if (c->pid <= 0 && c->next_restart_at > 0 && now >= c->next_restart_at) {
            c->next_restart_at = 0;
            pid_t pid = do_fork(c->name);
            if (pid > 0) {
                c->pid = pid;
                c->started_at = now;
                update_pid(db, c->name, pid);
                LOG_INFO_("channel respawn name=%s pid=%d",
                          c->name, (int)pid);
            }
        }

        /* Reset restart_count if healthy for 5 minutes */
        if (c->pid > 0 && c->restart_count > 0 &&
            c->started_at > 0 && (now - c->started_at) > CHANNEL_FLAP_WINDOW) {
            c->restart_count = 0;
            c->first_crash = 0;
        }
    }
}


void channel_consume_events(sqlite3 *db) {
    /* One event per pass, SELECT finalized before any write. Holding the
     * SELECT open across the loop body's writes (inbox, sessions, the delete
     * below) keeps a read snapshot open, and the write must then upgrade it.
     * A concurrent runner commit — telegram writes tg_offset right after
     * sending the wake byte — lands inside that snapshot and the upgrade
     * fails with an *immediate* SQLITE_BUSY (busy_timeout does not apply to
     * snapshot upgrades), parking the event until the next wake. The id
     * cursor also lets a failed event be skipped for this pass instead of
     * refetching it forever. */
    const char *sql = "SELECT id, channel_name, event_type, payload"
                      " FROM channel_events WHERE id > ? ORDER BY id ASC LIMIT 1;";
    int64_t cursor = 0;
    int processed = 0;

    for (;;) {
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) break;
        sqlite3_bind_int64(stmt, 1, cursor);
        if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); break; }
        int64_t eid = sqlite3_column_int64(stmt, 0);
        const char *v = (const char *)sqlite3_column_text(stmt, 1);
        char *ch_name = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 2);
        char *etype = v ? strdup(v) : NULL;
        v = (const char *)sqlite3_column_text(stmt, 3);
        char *payload = v ? strdup(v) : NULL;
        sqlite3_finalize(stmt);
        cursor = eid;

        if (!ch_name || !payload || !etype)
            goto del;

        /* Structural approval decision (inline-button tap, or any other
         * channel-specific UI for a yes/once/no answer). Channel-agnostic:
         * the daemon never interprets raw chat text as a decision — that's
         * entirely the channel extension's concern (see channel_telegram.qjs
         * processCallback). The approval carries its own session_id, so this
         * skips channel_routes/session lookup entirely. */
        if (strcmp(etype, "approval_decision") == 0) {
            processed++;
            int64_t approval_id = -1;
            char decision_str[16] = {0};
            const char *jsql =
                "SELECT json_extract(?1,'$.approval_id'), json_extract(?1,'$.decision');";
            sqlite3_stmt *js;
            if (sqlite3_prepare_v2(db, jsql, -1, &js, NULL) == SQLITE_OK) {
                sqlite3_bind_text(js, 1, payload, -1, SQLITE_STATIC);
                if (sqlite3_step(js) == SQLITE_ROW) {
                    approval_id = sqlite3_column_int64(js, 0);
                    const char *dv = (const char *)sqlite3_column_text(js, 1);
                    if (dv) snprintf(decision_str, sizeof(decision_str), "%s", dv);
                }
                sqlite3_finalize(js);
            }
            if (approval_id > 0 && decision_str[0]) {
                ApprovalDecision d = APPROVAL_DENY;
                if (strcmp(decision_str, "yes") == 0) d = APPROVAL_ALWAYS;
                else if (strcmp(decision_str, "once") == 0) d = APPROVAL_ONCE;
                char decided[128];
                snprintf(decided, sizeof(decided), "channel:%s", ch_name);
                resolve_approval(approval_id, d, decided, 0);
            } else {
                LOG_ERROR_("channel approval_decision malformed ch=%s eid=%lld payload=%s",
                           ch_name, (long long)eid, payload);
            }
            goto del;
        }

        if (strcmp(etype, "message") != 0)
            goto del;

        /* Resolve agent via channel_routes, find/create session */
        {
            /* Extract channel_id from payload via SQLite json_extract */
            const char *cid = "*";
            char cid_buf[64] = {0};
            {
                const char *jsql = "SELECT CAST(json_extract(?, '$.channel_id') AS TEXT);";
                sqlite3_stmt *js;
                if (sqlite3_prepare_v2(db, jsql, -1, &js, NULL) == SQLITE_OK) {
                    sqlite3_bind_text(js, 1, payload, -1, SQLITE_STATIC);
                    if (sqlite3_step(js) == SQLITE_ROW) {
                        const char *val = (const char *)sqlite3_column_text(js, 0);
                        if (val && val[0] && strlen(val) < sizeof(cid_buf)) {
                            memcpy(cid_buf, val, strlen(val) + 1);
                            cid = cid_buf;
                        }
                    }
                    sqlite3_finalize(js);
                }
            }

            char *agent = db_channel_binding_get(db, ch_name, cid);
            if (!agent && strcmp(cid, "*") != 0)
                agent = db_channel_binding_get(db, ch_name, "*");
            if (!agent) {
                /* No route: fall back to the default agent instead of
                 * silently deleting the message — a fresh DB has no
                 * channel_routes rows at all, and a dropped message with
                 * no trace is indistinguishable from a dead channel. */
                agent = config_get(db, "default_agent");
                if (agent)
                    LOG_INFO_("channel no route ch=%s cid=%s, using default_agent=%s",
                              ch_name, cid, agent);
                else
                    LOG_WARN_("channel no route ch=%s cid=%s and no default_agent"
                              " — dropping message", ch_name, cid);
            }
            if (!agent) goto del;

            /* Find or create session for this channel+channel_id */
            int64_t sid = -1;
            const char *ssql = "SELECT id FROM sessions WHERE channel_name=? AND channel_id=?"
                               " ORDER BY id DESC LIMIT 1;";
            sqlite3_stmt *ss;
            if (sqlite3_prepare_v2(db, ssql, -1, &ss, NULL) == SQLITE_OK) {
                sqlite3_bind_text(ss, 1, ch_name, -1, SQLITE_STATIC);
                sqlite3_bind_text(ss, 2, cid, -1, SQLITE_STATIC);
                if (sqlite3_step(ss) == SQLITE_ROW)
                    sid = sqlite3_column_int64(ss, 0);
                sqlite3_finalize(ss);
            }
            if (sid <= 0) {
                sid = session_create(db, ch_name, agent, -1, 0);
                if (sid > 0) {
                    LOG_INFO_("channel new_session ch=%s sid=%lld agent=%s",
                              ch_name, (long long)sid, agent);
                    /* Store channel_name + channel_id on session */
                    const char *usql = "UPDATE sessions SET channel_name=?, channel_id=? WHERE id=?;";
                    sqlite3_stmt *us;
                    if (sqlite3_prepare_v2(db, usql, -1, &us, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(us, 1, ch_name, -1, SQLITE_STATIC);
                        sqlite3_bind_text(us, 2, cid, -1, SQLITE_STATIC);
                        sqlite3_bind_int64(us, 3, sid);
                        sqlite3_step(us); sqlite3_finalize(us);
                    }
                }
            }
            if (sid > 0) {
                LOG_INFO_("channel event ch=%s sid=%lld type=%s",
                          ch_name, (long long)sid, etype);
                processed++;
                /* Every inbound chat message is forwarded as-is — approval
                 * decisions arrive as their own structural event type
                 * (approval_decision, above), never by interpreting chat
                 * text. A pending approval simply stays pending until a
                 * decision event resolves it (or it expires). */
                int64_t irc = inbox_insert_scanned(db, sid, ch_name, payload);
                if (irc < 0) {
                    /* Leave the row in channel_events (skip the del: below)
                     * so the next consume pass retries it — but log it, since
                     * a silent retry-forever with no trace is undebuggable. */
                    LOG_ERROR_("channel inbox_insert failed ch=%s sid=%lld eid=%lld, will retry",
                               ch_name, (long long)sid, (long long)eid);
                    free(agent);
                    free(ch_name); free(etype); free(payload);
                    continue;
                }
                wake_session(sid);
            }
            free(agent);
        }
del:;
        free(ch_name); free(etype); free(payload);
        const char *dsql = "DELETE FROM channel_events WHERE id=?;";
        sqlite3_stmt *ds;
        if (sqlite3_prepare_v2(db, dsql, -1, &ds, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ds, 1, eid);
            sqlite3_step(ds); sqlite3_finalize(ds);
        }
    }
    if (processed > 0)
        LOG_INFO_("channel consume_done count=%d", processed);
}

time_t channel_next_deadline(void) {
    time_t earliest = 0;
    for (int i = 0; i < g_count; i++) {
        time_t t = g_channels[i].next_restart_at;
        if (t > 0 && (earliest == 0 || t < earliest))
            earliest = t;
    }
    return earliest;
}
