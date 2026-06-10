#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "heartbeat.h"
#include "wake.h"
#include "db.h"
#include "types.h"

static void test_heartbeat_disabled(void) {
    Config cfg = {0};
    cfg.heartbeat_interval = 0;
    assert(heartbeat_start(&cfg, NULL) == 0);
    heartbeat_stop();
    printf("  PASS: heartbeat disabled\n");
}

static void test_heartbeat_injects_inbox(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    /* Init signal pipe so wake_session doesn't fail */
    assert(wake_init() == 0);

    /* Create a session, mark idle (default state) */
    int64_t sid = session_create(db, "test_hb", NULL, -1, 0);
    assert(sid > 0);
    /* Touch updated_at so session is "recently active" */
    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    /* Start heartbeat with 1s interval */
    Config cfg = {0};
    cfg.heartbeat_interval = 1;
    assert(heartbeat_start(&cfg, db) == 0);

    /* Wait for at least one heartbeat to fire */
    sleep(2);
    heartbeat_stop();

    /* V42: Check that heartbeat prompt was inserted into inbox */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count >= 1);

    int found = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(items[i].source, "heartbeat") == 0 &&
            strstr(items[i].payload, "HEARTBEAT_OK")) {
            found = 1;
            break;
        }
    }
    assert(found);

    /* Drain signal pipe to verify session was signaled */
    int fd = wake_fd();
    int64_t signaled_sid = 0;
    ssize_t n = read(fd, &signaled_sid, sizeof(signaled_sid));
    assert(n == sizeof(signaled_sid));
    assert(signaled_sid == sid);

    free(items);
    wake_close();
    db_close(db);
    printf("  PASS: heartbeat injects into inbox + signals daemon\n");
}

static void test_heartbeat_skips_non_idle(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    assert(wake_init() == 0);

    /* Create session and set state to running */
    int64_t sid = session_create(db, "test_hb_running", NULL, -1, 0);
    assert(sid > 0);
    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    /* Force state to running */
    char sql[128];
    snprintf(sql, sizeof(sql),
        "UPDATE sessions SET state='running' WHERE id=%lld", (long long)sid);
    sqlite3_exec(db, sql, NULL, NULL, NULL);

    Config cfg = {0};
    cfg.heartbeat_interval = 1;
    assert(heartbeat_start(&cfg, db) == 0);
    sleep(2);
    heartbeat_stop();

    /* Should NOT have inserted into inbox */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 0);
    free(items);

    wake_close();
    db_close(db);
    printf("  PASS: heartbeat skips non-idle sessions\n");
}

int main(void) {
    printf("test_heartbeat:\n");
    test_heartbeat_disabled();
    test_heartbeat_injects_inbox();
    test_heartbeat_skips_non_idle();
    printf("ALL PASSED\n");
    return 0;
}
