#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "channel.h"
#include "approval.h"
#include "db.h"
#include "secret_scan.h"
#include "wake.h"
#include "resolve.h"
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
        /* Re-exec self; the --channel branch resolves channel_runner
         * relative to the binary, never the cwd. */
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


int channel_launch_all(sqlite3 *db) {
    const char *sql = "SELECT c.name, e.path FROM channels c"
                      " JOIN extensions e ON c.extension_name=e.name"
                      " WHERE c.status='active';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int launched = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && g_count < CHANNEL_MAX) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *ext_path = (const char *)sqlite3_column_text(stmt, 1);
        if (!name || !ext_path) continue;

        /* Build js_path: <ext_path>/channel.js */
        char js_path[1024];
        snprintf(js_path, sizeof(js_path), "%s/channel.js", ext_path);

        pid_t pid = do_fork(name);
        if (pid > 0) {
            ChannelProc *c = &g_channels[g_count++];
            c->pid = pid;
            c->restart_count = 0;
            c->first_crash = 0;
            c->next_restart_at = 0;
            c->started_at = time(NULL);
            snprintf(c->name, sizeof(c->name), "%s", name);
            update_pid(db, name, pid);
            launched++;
        }
    }
    sqlite3_finalize(stmt);
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
        fprintf(stderr, "channel '%s': flapping (%d crashes in %lds), marking broken\n",
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


int channel_register(sqlite3 *db, const char *name, const char *extension_name) {
    const char *sql =
        "INSERT OR REPLACE INTO channels(name, extension_name, status) VALUES(?,?,'active');";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, extension_name, -1, SQLITE_STATIC);
    int rc = (sqlite3_step(s) == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(s);
    return rc;
}

void channel_consume_events(sqlite3 *db) {
    const char *sql = "SELECT id, channel_name, event_type, payload"
                      " FROM channel_events ORDER BY id ASC;";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int64_t eid = sqlite3_column_int64(stmt, 0);
        const char *ch_name = (const char *)sqlite3_column_text(stmt, 1);
        const char *etype = (const char *)sqlite3_column_text(stmt, 2);
        const char *payload = (const char *)sqlite3_column_text(stmt, 3);

        if (!ch_name || !payload || !etype || strcmp(etype, "message") != 0)
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
                /* Store channel_name + channel_id on session */
                if (sid > 0) {
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
                /* Check if session is awaiting approval — route as decision */
                Approval *pa = approval_get_pending(db, sid);
                if (pa) {
                    /* Extract text from payload */
                    const char *tsql = "SELECT json_extract(?, '$.text');";
                    sqlite3_stmt *ts;
                    char *text = NULL;
                    if (sqlite3_prepare_v2(db, tsql, -1, &ts, NULL) == SQLITE_OK) {
                        sqlite3_bind_text(ts, 1, payload, -1, SQLITE_STATIC);
                        if (sqlite3_step(ts) == SQLITE_ROW) {
                            const char *tv = (const char *)sqlite3_column_text(ts, 0);
                            if (tv) text = strdup(tv);
                        }
                        sqlite3_finalize(ts);
                    }
                    int is_approve = 0;
                    if (text) {
                        is_approve = (text[0] == 'y' || text[0] == 'Y' ||
                                      strcasecmp(text, "approve") == 0);
                        free(text);
                    }
                    char decided[128];
                    snprintf(decided, sizeof(decided), "channel:%s", ch_name);
                    resolve_approval(pa->id, is_approve ? APPROVAL_ALWAYS : APPROVAL_DENY, decided);
                    approval_free(pa);
                    wake_session(sid);
                } else {
                    int64_t irc = inbox_insert_scanned(db, sid, ch_name, payload);
                    if (irc < 0) {
                        free(agent);
                        continue;
                    }
                    wake_session(sid);
                }
            }
            free(agent);
        }
del:;
        const char *dsql = "DELETE FROM channel_events WHERE id=?;";
        sqlite3_stmt *ds;
        if (sqlite3_prepare_v2(db, dsql, -1, &ds, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(ds, 1, eid);
            sqlite3_step(ds); sqlite3_finalize(ds);
        }
    }
    sqlite3_finalize(stmt);
}
