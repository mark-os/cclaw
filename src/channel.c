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

static pid_t do_fork(const char *binary_path, const char *db_path, const char *name) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl(binary_path, binary_path, db_path, name, (char *)NULL);
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
    const char *sql = "SELECT name, binary_path FROM channels WHERE status='active';";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return 0;
    int launched = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && g_count < CHANNEL_MAX) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *bpath = (const char *)sqlite3_column_text(stmt, 1);
        if (!name || !bpath) continue;
        pid_t pid = do_fork(bpath, db_path, name);
        if (pid > 0) {
            ChannelProc *c = &g_channels[g_count++];
            c->pid = pid;
            c->restart_count = 0;
            snprintf(c->name, sizeof(c->name), "%s", name);
            snprintf(c->binary_path, sizeof(c->binary_path), "%s", bpath);
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
    pid_t new_pid = do_fork(c->binary_path, db_path, c->name);
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

int channel_register(sqlite3 *db, const char *name, const char *binary_path) {
    const char *sql =
        "INSERT OR REPLACE INTO channels(name, type, binary_path, status) VALUES(?,?,?,'active');";
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, name, -1, SQLITE_STATIC); /* type = name */
    sqlite3_bind_text(s, 3, binary_path, -1, SQLITE_STATIC);
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

        /* Resolve agent + session, insert inbox, wake */
        {
            char *agent = db_channel_binding_get(db, ch_name, "default");
            if (!agent) goto del;

            /* Find or create session via channel_state */
            int64_t sid = -1;
            char key[128];
            snprintf(key, sizeof(key), "session:%s", ch_name);
            char *sv = db_kv_get(db, key);
            if (sv) { sid = strtoll(sv, NULL, 10); free(sv); }
            if (sid <= 0) {
                sid = session_create(db, ch_name, agent, -1, 0);
                if (sid > 0) { char buf[32]; snprintf(buf, sizeof(buf), "%lld", (long long)sid);
                    db_kv_set(db, key, buf); }
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
