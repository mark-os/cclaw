#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "db.h"
#include "llm.h"
#include "test_util.h"
#include "types.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

#define ASSERT_EQ(got, want) do { \
    if ((got) != (want)) { FAIL(#got " != " #want); return; } \
} while(0)

static void test_stop_variants(void) {
    TEST(stop_variants);
    ASSERT_EQ(map_stop_reason("stop"), STOP_REASON_STOP);
    ASSERT_EQ(map_stop_reason("end"), STOP_REASON_STOP);
    ASSERT_EQ(map_stop_reason("end_turn"), STOP_REASON_STOP);
    ASSERT_EQ(map_stop_reason(NULL), STOP_REASON_STOP);
    PASS();
}

static void test_length_variants(void) {
    TEST(length_variants);
    ASSERT_EQ(map_stop_reason("length"), STOP_REASON_LENGTH);
    ASSERT_EQ(map_stop_reason("max_tokens"), STOP_REASON_LENGTH);
    PASS();
}

static void test_tool_use_variants(void) {
    TEST(tool_use_variants);
    ASSERT_EQ(map_stop_reason("tool_calls"), STOP_REASON_TOOL_USE);
    ASSERT_EQ(map_stop_reason("function_call"), STOP_REASON_TOOL_USE);
    ASSERT_EQ(map_stop_reason("tool_use"), STOP_REASON_TOOL_USE);
    PASS();
}

static void test_error_variants(void) {
    TEST(error_variants);
    ASSERT_EQ(map_stop_reason("content_filter"), STOP_REASON_ERROR);
    ASSERT_EQ(map_stop_reason("network_error"), STOP_REASON_ERROR);
    ASSERT_EQ(map_stop_reason("something_unknown"), STOP_REASON_ERROR);
    PASS();
}
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

/* A length-stopped answer must announce itself on the delivery path; every
 * other stop reason renders the model's bytes untouched. */
static void test_length_notice_rendered(void) {
    TEST(length_notice_rendered);

    char path[] = "/tmp/cclaw_test_sr4_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) FAIL("mkstemp");
    close(fd);

    sqlite3 *db = test_db_open(path);
    if (!db) { test_db_clean(path); FAIL("db_open"); }

    int64_t sid = session_create(db, "test_sr4", NULL, -1, 0);
    if (sid < 0) { db_close(db); test_db_clean(path); FAIL("session_create"); }

    Message cut = {.role = ROLE_ASSISTANT,
                   .content = strdup("here is the first half of the ans"),
                   .stop_reason = STOP_REASON_LENGTH};
    entry_append_with_turn(db, sid, &cut, db_next_turn_id(db, sid));
    free(cut.content);

    char *text = get_response_text(db, sid);
    if (!text) { db_close(db); test_db_clean(path); FAIL("no text"); }
    if (!strstr(text, "here is the first half of the ans")) {
        free(text); db_close(db); test_db_clean(path);
        FAIL("model text lost");
    }
    if (!strstr(text, RESPONSE_TRUNCATED_NOTICE)) {
        free(text); db_close(db); test_db_clean(path);
        FAIL("missing truncation notice");
    }
    free(text);

    /* The notice is render-only: the stored entry keeps the model's bytes. */
    char *stored = db_scalar_text(db,
        "SELECT content FROM entries WHERE session_id=? ORDER BY id DESC LIMIT 1;", sid);
    if (!stored || strcmp(stored, "here is the first half of the ans") != 0) {
        free(stored); db_close(db); test_db_clean(path);
        FAIL("entry content was mutated");
    }
    free(stored);

    /* A normal stop renders clean. */
    Message done = {.role = ROLE_ASSISTANT,
                    .content = strdup("all done"),
                    .stop_reason = STOP_REASON_STOP};
    entry_append_with_turn(db, sid, &done, db_next_turn_id(db, sid));
    free(done.content);

    text = get_response_text(db, sid);
    if (!text || strcmp(text, "all done") != 0) {
        free(text); db_close(db); test_db_clean(path);
        FAIL("normal stop should render verbatim");
    }
    free(text);

    db_close(db);
    test_db_clean(path);
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("--- test_stop_reason_persist ---\n");
    test_stop_variants();
    test_length_variants();
    test_tool_use_variants();
    test_error_variants();
    test_stop_reason_roundtrip();
    test_stop_reason_none_not_stored();
    test_all_stop_reasons();
    test_length_notice_rendered();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
