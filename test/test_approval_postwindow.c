/* Test approval_deliver_postwindow: each outcome inserts an inbox row */
#define _POSIX_C_SOURCE 200809L
#include "approval.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_approval_postwindow.db"

static void clean_db(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    return db;
}

static void test_postwindow_outcomes(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);

    /* One session per outcome to avoid inbox cross-contamination */
    struct {
        ApprovalPostWindow outcome;
        const char *substr;
    } cases[] = {
        { APPROVAL_PW_RERUN_APPROVED,  "re-issue" },
        { APPROVAL_PW_RERUN_DENIED,    "denied"   },
        { APPROVAL_PW_APPLY_GRANTED,   "granted"  },
        { APPROVAL_PW_APPLY_DENIED,    "denied"   },
        { APPROVAL_PW_EXPIRED,         "expired"  },
    };

    for (int i = 0; i < 5; i++) {
        int64_t sid = session_create(db, "test", "bot", -1, 0);
        assert(sid > 0);

        int64_t aid = approval_create(db, sid, "call_pw", "shell_exec",
                                      "run_cmd", "{\"cmd\":\"ls\"}", "rerun");
        assert(aid > 0);

        Approval *a = approval_get_pending(db, sid);
        assert(a != NULL);

        int64_t inbox_id = approval_deliver_postwindow(db, a, cases[i].outcome);
        assert(inbox_id > 0);
        approval_free(a);

        /* Verify inbox row */
        int count = 0;
        InboxItem *items = inbox_peek(db, sid, 10, &count);
        assert(count == 1);
        assert(strcmp(items[0].source, "approval") == 0);
        assert(strstr(items[0].payload, cases[i].substr) != NULL);
        inbox_items_free(items, count);
    }

    db_close(db);
    clean_db();
    printf("  PASS test_postwindow_outcomes\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_approval_postwindow:\n");
    test_postwindow_outcomes();
    printf("all approval postwindow tests passed\n");
    return 0;
}
