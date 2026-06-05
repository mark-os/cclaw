#include "daemon.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define WORK_DIR "/tmp/test_daemon_inbox_work"
#define AGENT_NAME "test-agent"

static char orig_cwd[1024];

static void setup(void) {
    getcwd(orig_cwd, sizeof(orig_cwd));
    system("rm -rf " WORK_DIR);
    mkdir(WORK_DIR, 0755);
    chdir(WORK_DIR);

    /* T297: single DB — set daemon db path and create it */
    daemon_set_db_path("cclaw.db");
    sqlite3 *db = db_open("cclaw.db");
    assert(db != NULL);
    db_close(db);
}

static void teardown(void) {
    chdir(orig_cwd);
    system("rm -rf " WORK_DIR);
}

static int test_insert_and_count(void) {
    setup();

    /* Insert via daemon_inbox_insert */
    int64_t id = daemon_inbox_insert(AGENT_NAME, 1, "telegram", "hello");
    assert(id > 0);

    /* Count via daemon_inbox_count */
    int count = daemon_inbox_count(AGENT_NAME, 1);
    assert(count == 1);

    /* Insert another */
    id = daemon_inbox_insert(AGENT_NAME, 1, "cron", "task");
    assert(id > 0);
    count = daemon_inbox_count(AGENT_NAME, 1);
    assert(count == 2);

    /* Different session — should be 0 */
    count = daemon_inbox_count(AGENT_NAME, 99);
    assert(count == 0);

    teardown();
    printf("  PASS test_insert_and_count\n");
    return 0;
}

static int test_null_agent_returns_error(void) {
    setup();

    int64_t id = daemon_inbox_insert(NULL, 1, "test", "msg");
    assert(id == -1);

    int count = daemon_inbox_count(NULL, 1);
    assert(count == -1);

    teardown();
    printf("  PASS test_null_agent_returns_error\n");
    return 0;
}

static int test_nonexistent_agent_still_works(void) {
    setup();

    /* T297: single DB — any agent name writes to same DB */
    int64_t id = daemon_inbox_insert("nonexistent", 1, "test", "msg");
    assert(id > 0);

    int count = daemon_inbox_count("nonexistent", 1);
    assert(count == 1);

    teardown();
    printf("  PASS test_nonexistent_agent_still_works\n");
    return 0;
}

static int test_verify_in_agent_db(void) {
    setup();

    daemon_inbox_insert(AGENT_NAME, 42, "sub-agent", "result data");

    /* T297: verify in same cclaw.db */
    sqlite3 *adb = db_open("cclaw.db");
    assert(adb != NULL);

    int count = 0;
    InboxItem *items = inbox_peek(adb, 42, 10, &count);
    assert(count == 1);
    assert(items != NULL);
    assert(strcmp(items[0].source, "sub-agent") == 0);
    assert(strcmp(items[0].payload, "result data") == 0);
    assert(items[0].session_id == 42);

    inbox_items_free(items, count);
    db_close(adb);
    teardown();
    printf("  PASS test_verify_in_agent_db\n");
    return 0;
}

int main(void) {
    printf("test_daemon_inbox:\n");
    int rc = 0;
    rc |= test_insert_and_count();
    rc |= test_null_agent_returns_error();
    rc |= test_nonexistent_agent_still_works();
    rc |= test_verify_in_agent_db();
    if (rc == 0) printf("All daemon inbox tests passed.\n");
    return rc;
}
