#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "channel.h"
#include "db.h"
#include "wake.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static pid_t do_fork(const char *db_path, const char *name) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/proc/self/exe", "cclaw", "--channel", name, (char *)NULL);
        /* Fallback: try channel_runner directly */
        execl("build/channel_runner", "channel_runner", db_path, name, (char *)NULL);
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

static void set_status(sqlite3 *db, const char *name, const char *status) {
    const char *sql = "UPDATE channels SET status=? WHERE name=?;";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) == SQLITE_OK) {
        sqlite3_bind_text(s, 1, status, -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC);
        sqlite3_step(s); sqlite3_finalize(s);
    }
}

int channel_launch_all(sqlite3 *db, const char *db_path) {
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

        pid_t pid = do_fork(db_path, name);
        if (pid > 0) {
            ChannelProc *c = &g_channels[g_count++];
            c->pid = pid;
            c->restart_count = 0;
            snprintf(c->name, sizeof(c->name), "%s", name);
            snprintf(c->binary_path, sizeof(c->binary_path), "/proc/self/exe");
            update_pid(db, name, pid);
            launched++;
        }
    }
    sqlite3_finalize(stmt);
    return launched;
}

int channel_reap(pid_t pid, int status, sqlite3 *db, const char *db_path) {
    ChannelProc *c = find_by_pid(pid);
    if (!c) return 0;
    (void)status;

    if (c->restart_count >= CHANNEL_MAX_RESTARTS) {
        set_status(db, c->name, "failed");
        update_pid(db, c->name, 0);
        remove_channel(c);
        return 1;
    }

    c->restart_count++;
    pid_t new_pid = do_fork(db_path, c->name);
    if (new_pid > 0) {
        c->pid = new_pid;
        update_pid(db, c->name, new_pid);
    } else {
        remove_channel(c);
    }
    return 1;
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
            /* Extract channel_id from payload if present */
            const char *cid = "*";
            char cid_buf[64] = {0};
            const char *cid_key = "\"channel_id\":\"";
            const char *p = strstr(payload, cid_key);
            if (p) {
                p += strlen(cid_key);
                const char *end = strchr(p, '"');
                if (end && (size_t)(end - p) < sizeof(cid_buf)) {
                    memcpy(cid_buf, p, (size_t)(end - p));
                    cid_buf[end - p] = '\0';
                    cid = cid_buf;
                }
            }

            char *agent = db_channel_binding_get(db, ch_name, cid);
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
                inbox_insert(db, sid, ch_name, payload);
                wake_session(sid);
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
