#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "heartbeat.h"
#include "db.h"
#include "types.h"

static void test_heartbeat_disabled(void) {
    Config cfg = {0};
    cfg.heartbeat_interval = 0;
    /* Should return 0 (no-op) when disabled */
    assert(heartbeat_start(&cfg, NULL) == 0);
    heartbeat_stop();
    printf("  PASS: heartbeat disabled\n");
}

static void test_heartbeat_injects(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    /* Create a session and mark it active */
    int64_t sid = session_create(db, "test_hb");
    assert(sid > 0);
    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);

    /* Start heartbeat with 1s interval */
    Config cfg = {0};
    cfg.heartbeat_interval = 1;
    assert(heartbeat_start(&cfg, db) == 0);

    /* Wait for at least one heartbeat to fire */
    sleep(2);
    heartbeat_stop();

    /* Check that a system heartbeat message was injected */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    assert(entries);
    assert(count >= 2); /* user + at least 1 heartbeat system msg */

    int found_heartbeat = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].message.role == ROLE_SYSTEM &&
            entries[i].message.content &&
            strstr(entries[i].message.content, "[heartbeat ")) {
            found_heartbeat = 1;
            break;
        }
    }
    assert(found_heartbeat);
    entry_branch_free(entries, count);
    db_close(db);
    printf("  PASS: heartbeat injects system msg\n");
}

int main(void) {
    printf("test_heartbeat:\n");
    test_heartbeat_disabled();
    test_heartbeat_injects();
    printf("ALL PASSED\n");
    return 0;
}
