#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "db.h"
#include "test_util.h"
#include "types.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void test_stop_reason_roundtrip(void) {
    TEST(stop_reason_roundtrip);

    /* Create temp DB */
    char path[] = "/tmp/cclaw_test_sr_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) FAIL("mkstemp");
    close(fd);

    sqlite3 *db = test_db_open(path);
    if (!db) { test_db_clean(path); FAIL("db_open"); }

    int64_t sid = session_create(db, "test_sr", NULL, -1, 0);
    if (sid < 0) { db_close(db); test_db_clean(path); FAIL("session_create"); }

    /* Append assistant message with stop_reason = STOP_REASON_TOOL_USE */
    Message msg = {.role = ROLE_ASSISTANT,
                   .content = strdup("thinking..."),
                   .stop_reason = STOP_REASON_TOOL_USE};
    int64_t turn_id = db_next_turn_id(db, sid);
    int64_t eid = entry_append_with_turn(db, sid, &msg, turn_id);
    free(msg.content);
    if (eid < 0) { db_close(db); test_db_clean(path); FAIL("entry_append"); }

    /* Read back via session_get_branch */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 1) { db_close(db); test_db_clean(path); FAIL("get_branch count"); }

    if (entries[0].message.stop_reason != STOP_REASON_TOOL_USE) {
        entry_branch_free(entries, count);
        db_close(db); test_db_clean(path);
        FAIL("stop_reason mismatch");
    }

    entry_branch_free(entries, count);
    db_close(db);
    test_db_clean(path);
    PASS();
}

static void test_stop_reason_none_not_stored(void) {
    TEST(stop_reason_none_not_stored);

    char path[] = "/tmp/cclaw_test_sr2_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) FAIL("mkstemp");
    close(fd);

    sqlite3 *db = test_db_open(path);
    if (!db) { test_db_clean(path); FAIL("db_open"); }

    int64_t sid = session_create(db, "test_sr2", NULL, -1, 0);
    if (sid < 0) { db_close(db); test_db_clean(path); FAIL("session_create"); }

    /* Append user message (no stop_reason) */
    Message msg = {.role = ROLE_USER, .content = strdup("hello")};
    int64_t turn_id = db_next_turn_id(db, sid);
    entry_append_with_turn(db, sid, &msg, turn_id);
    free(msg.content);

    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != 1) { db_close(db); test_db_clean(path); FAIL("get_branch"); }

    if (entries[0].message.stop_reason != STOP_REASON_NONE) {
        entry_branch_free(entries, count);
        db_close(db); test_db_clean(path);
        FAIL("expected NONE");
    }

    entry_branch_free(entries, count);
    db_close(db);
    test_db_clean(path);
    PASS();
}

static void test_all_stop_reasons(void) {
    TEST(all_stop_reasons_persist);

    char path[] = "/tmp/cclaw_test_sr3_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) FAIL("mkstemp");
    close(fd);

    sqlite3 *db = test_db_open(path);
    if (!db) { test_db_clean(path); FAIL("db_open"); }

    int64_t sid = session_create(db, "test_sr3", NULL, -1, 0);
    if (sid < 0) { db_close(db); test_db_clean(path); FAIL("session_create"); }

    StopReason reasons[] = {STOP_REASON_STOP, STOP_REASON_LENGTH,
                            STOP_REASON_TOOL_USE, STOP_REASON_ERROR,
                            STOP_REASON_ABORTED};
    int n = sizeof(reasons) / sizeof(reasons[0]);

    for (int i = 0; i < n; i++) {
        Message msg = {.role = ROLE_ASSISTANT,
                       .content = strdup("msg"),
                       .stop_reason = reasons[i]};
        int64_t turn_id = db_next_turn_id(db, sid);
        entry_append_with_turn(db, sid, &msg, turn_id);
        free(msg.content);
    }

    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    if (!entries || count != n) { db_close(db); test_db_clean(path); FAIL("count mismatch"); }

    for (int i = 0; i < n; i++) {
        if (entries[i].message.stop_reason != reasons[i]) {
            entry_branch_free(entries, count);
            db_close(db); test_db_clean(path);
            FAIL("reason mismatch at index");
        }
    }

    entry_branch_free(entries, count);
    db_close(db);
    test_db_clean(path);
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("--- test_stop_reason_persist ---\n");
    test_stop_reason_roundtrip();
    test_stop_reason_none_not_stored();
    test_all_stop_reasons();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
