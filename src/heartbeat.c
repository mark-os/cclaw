#define _POSIX_C_SOURCE 200809L
#include "heartbeat.h"
#include "db.h"
#include "wake.h"
#include "db.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static pthread_t hb_thread;
static volatile int hb_running;
static const Config *hb_cfg;
static sqlite3 *hb_db;

#define HEARTBEAT_PROMPT \
    "Read HEARTBEAT.md if present. Follow it. " \
    "If nothing needs attention, reply HEARTBEAT_OK."

/* Insert heartbeat prompt into inbox for idle sessions, signal daemon.
 * Collect session ids first, then finalize the reader before writing —
 * an open SELECT pins a WAL read snapshot and any write that requires a
 * snapshot upgrade gets an immediate SQLITE_BUSY (busy_timeout won't help). */
#define HB_MAX_SESSIONS 256
static void inject_heartbeat(void) {
    int interval = hb_cfg->heartbeat_interval;
    char sql[256];
    snprintf(sql, sizeof(sql),
        "SELECT id FROM sessions WHERE state='idle' "
        "AND updated_at >= unixepoch() - %d", interval);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(hb_db, sql, -1, &stmt, NULL) != SQLITE_OK) return;

    int64_t sids[HB_MAX_SESSIONS];
    int n = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW && n < HB_MAX_SESSIONS) {
        sids[n++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    for (int i = 0; i < n; i++) {
        inbox_insert(hb_db, sids[i], "heartbeat", HEARTBEAT_PROMPT);
        wake_session(sids[i]);
    }
}

static void *heartbeat_loop(void *arg) {
    (void)arg;
    while (hb_running) {
        for (int i = 0; i < hb_cfg->heartbeat_interval && hb_running; i++)
            sleep(1);
        if (hb_running)
            inject_heartbeat();
    }
    return NULL;
}

int heartbeat_start(const Config *cfg, sqlite3 *db) {
    if (!cfg || cfg->heartbeat_interval <= 0) return 0;

    hb_cfg = cfg;
    hb_db = db;
    hb_running = 1;

    if (pthread_create(&hb_thread, NULL, heartbeat_loop, NULL) != 0) {
        hb_running = 0;
        return -1;
    }
    return 0;
}

void heartbeat_stop(void) {
    if (!hb_running) return;
    hb_running = 0;
    pthread_join(hb_thread, NULL);
}
