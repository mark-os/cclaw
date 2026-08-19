/* Test approval_list_block_due: block window selection and owner filtering */
#define _POSIX_C_SOURCE 200809L
#include "approval.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_approval_block_window.db"

static void clean_db(void) {
    test_db_clean(DB_PATH);
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    return db;
}

static void test_block_window_timing(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);

    /* Register our instance so the session is owned by us */
    char my_id[64];
    assert(process_register(db, "cli", getpid(), my_id, sizeof(my_id)) == 0);
    db_set_instance_id(my_id);

    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);

    /* Drive session to awaiting_approval: idle → llm_running → awaiting_approval */
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "awaiting_approval") == 0);

    int64_t aid = approval_create(db, sid, "call_1", "shell_exec",
                                  APPROVAL_PARK_REQUIRED, "{\"cmd\":\"ls\"}", "rerun");
    assert(aid > 0);

    /* With large block_sec, NOT due */
    int count = 0;
    int64_t *ids = approval_list_block_due(db, 3600, my_id, &count);
    assert(count == 0);
    if (ids) free(ids);

    /* Backdate requested_at so it's past block window */
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE approvals SET requested_at = unixepoch() - 100 WHERE id = %lld;",
        (long long)aid);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    /* With small block_sec, IS due */
    ids = approval_list_block_due(db, 2, my_id, &count);
    assert(count == 1);
    assert(ids[0] == aid);
    free(ids);

    process_unregister(db, my_id);
    db_set_instance_id(NULL);
    db_close(db);
    clean_db();
    printf("  PASS test_block_window_timing\n");
}

static void test_block_window_owner_filter(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);

    /* Register instance A (the one we query as "me") */
    char id_a[64];
    assert(process_register(db, "cli", getpid(), id_a, sizeof(id_a)) == 0);

    /* Register instance B (another live process — use same pid, it's fine for the test) */
    char id_b[64];
    assert(process_register(db, "daemon", getpid(), id_b, sizeof(id_b)) == 0);

    /* Create session owned by B */
    db_set_instance_id(id_b);
    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "awaiting_approval") == 0);

    int64_t aid = approval_create(db, sid, "call_2", "shell_exec",
                                  APPROVAL_PARK_REQUIRED, "{\"cmd\":\"ls\"}", "rerun");
    assert(aid > 0);

    /* Backdate so it's past block window */
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE approvals SET requested_at = unixepoch() - 100 WHERE id = %lld;",
        (long long)aid);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    /* As me=id_a, the session is owned by live instance id_b → excluded */
    int count = 0;
    int64_t *ids = approval_list_block_due(db, 2, id_a, &count);
    assert(count == 0);
    if (ids) free(ids);

    /* As me=id_b (the owner), it IS returned */
    ids = approval_list_block_due(db, 2, id_b, &count);
    assert(count == 1);
    assert(ids[0] == aid);
    free(ids);

    process_unregister(db, id_a);
    process_unregister(db, id_b);
    db_set_instance_id(NULL);
    db_close(db);
    clean_db();
    printf("  PASS test_block_window_owner_filter\n");
}

int main(void) {
    TEST_INIT();
    printf("test_approval_block_window:\n");
    test_block_window_timing();
    test_block_window_owner_filter();
    printf("all approval block window tests passed\n");
    return 0;
}
