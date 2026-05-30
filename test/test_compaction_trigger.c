#include "context.h"
#include "db.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_cclaw_compact_trigger.sqlite"

static sqlite3 *setup(void) {
    unlink(TEST_DB);
    return db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    unlink(TEST_DB);
}

/* T161: compaction triggers when branch exceeds budget + threshold */
static void test_trigger_fires(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "trigger_test", NULL, -1, 0);

    /* Config: small context window so 20 entries (~200 tokens) exceed threshold */
    Config cfg = {0};
    cfg.context_threshold = 0.1f; cfg.compaction_target = 0.05f; cfg.compaction = 1;
    cfg.provider.context_window = 500; /* 0.1 × 500 = 50 token trigger */
    cfg.provider.model = "test";
    cfg.provider.max_tokens = 100;

    /* Insert 20 entries — each ~10 tokens (40 chars) */
    char buf[64];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "message number %02d padding text here", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append(db, sid, &m);
    }

    /* Should need compaction (many entries beyond budget) */
    assert(session_needs_compaction(db, sid, &cfg) == 1);

    /* Trigger compaction */
    int64_t cid = session_try_compact(db, sid, &cfg);
    assert(cid > 0);

    /* After compaction, branch should be shorter (compaction node inserted) */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    /* Should have: first entry + compaction node + remaining entries */
    assert(count < 20);

    /* Verify compaction entry exists in branch */
    int found_compaction = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_COMPACTION) {
            found_compaction = 1;
            assert(strstr(branch[i].message.content, "Compacted") != NULL);
            break;
        }
    }
    assert(found_compaction);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_trigger_fires\n");
}

/* T161: no compaction when below threshold */
static void test_no_trigger_below_threshold(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "no_trigger", NULL, -1, 0);

    Config cfg = {0};
    cfg.context_threshold = 0.9f; cfg.compaction_target = 0.5f; cfg.compaction = 1;
    cfg.provider.context_window = 1000000; /* huge — 10 short entries won't exceed 900k */

    /* Insert 10 entries — well within budget */
    for (int i = 0; i < 10; i++) {
        Message m = {.role = ROLE_USER, .content = "short"};
        entry_append(db, sid, &m);
    }

    assert(session_needs_compaction(db, sid, &cfg) == 0);
    int64_t cid = session_try_compact(db, sid, &cfg);
    assert(cid == 0); /* no compaction performed */

    teardown(db);
    printf("  PASS test_no_trigger_below_threshold\n");
}

/* T161: CTE from leaf stops at compaction node after trigger */
static void test_trigger_cte_correct(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "cte_test", NULL, -1, 0);

    Config cfg = {0};
    cfg.context_threshold = 0.1f; cfg.compaction_target = 0.05f; cfg.compaction = 1;
    cfg.provider.context_window = 300; /* 0.1 × 300 = 30 token trigger */
    cfg.provider.model = "test";
    cfg.provider.max_tokens = 100;

    /* Insert 15 entries */
    char buf[64];
    for (int i = 0; i < 15; i++) {
        snprintf(buf, sizeof(buf), "entry_%02d with some padding text", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append(db, sid, &m);
    }

    int64_t cid = session_try_compact(db, sid, &cfg);
    assert(cid > 0);

    /* Verify branch structure: root → compaction → remaining */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    assert(count >= 3); /* at least root + compaction + one entry */
    assert(branch[0].message.role == ROLE_USER); /* root entry */
    assert(branch[1].message.role == ROLE_COMPACTION);
    assert(branch[1].id == cid);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_trigger_cte_correct\n");
}

int main(void) {
    printf("test_compaction_trigger:\n");
    test_trigger_fires();
    test_no_trigger_below_threshold();
    test_trigger_cte_correct();
    printf("All compaction trigger tests passed.\n");
    return 0;
}
