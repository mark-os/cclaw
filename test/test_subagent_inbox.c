#include "db.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_subagent_inbox.sqlite";

/* V13: background sub-agent posts completion to parent inbox */
static void test_subagent_completion_posts_to_parent_inbox(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t parent_sid = session_create(db, "parent", NULL);
    assert(parent_sid > 0);
    int64_t child_sid = session_create(db, "sub-agent:1", NULL);
    assert(child_sid > 0);

    /* Simulate spawn: create sub-agent record */
    int64_t agent_id = subagent_create(db, parent_sid, child_sid, 12345, 1, "test task");
    assert(agent_id > 0);

    /* Simulate sub-agent finishing and posting to parent inbox (V13) */
    subagent_finish(db, agent_id, "done", "task completed");
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"sub_agent_done\",\"agent_id\":%lld,\"session_id\":%lld,\"status\":\"done\"}",
             (long long)agent_id, (long long)child_sid);
    int64_t inbox_id = inbox_insert(db, parent_sid, "sub_agent", payload);
    assert(inbox_id > 0);

    /* Verify parent inbox has the notification */
    int count = 0;
    InboxItem *items = inbox_peek(db, parent_sid, 10, &count);
    assert(count == 1);
    assert(items != NULL);
    assert(strcmp(items[0].source, "sub_agent") == 0);
    assert(strstr(items[0].payload, "sub_agent_done") != NULL);
    assert(strstr(items[0].payload, "\"status\":\"done\"") != NULL);
    inbox_items_free(items, count);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_subagent_completion_posts_to_parent_inbox\n");
}

/* V13: error sub-agent also posts to parent inbox */
static void test_subagent_error_posts_to_parent_inbox(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t parent_sid = session_create(db, "parent2", NULL);
    assert(parent_sid > 0);
    int64_t child_sid = session_create(db, "sub-agent:2", NULL);
    assert(child_sid > 0);

    int64_t agent_id = subagent_create(db, parent_sid, child_sid, 99999, 1, "failing task");
    assert(agent_id > 0);

    subagent_finish(db, agent_id, "error", "no response");
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"event\":\"sub_agent_done\",\"agent_id\":%lld,\"session_id\":%lld,\"status\":\"error\"}",
             (long long)agent_id, (long long)child_sid);
    inbox_insert(db, parent_sid, "sub_agent", payload);

    int count = 0;
    InboxItem *items = inbox_peek(db, parent_sid, 10, &count);
    assert(count == 1);
    assert(strstr(items[0].payload, "\"status\":\"error\"") != NULL);
    inbox_items_free(items, count);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_subagent_error_posts_to_parent_inbox\n");
}

int main(void) {
    printf("test_subagent_inbox:\n");
    test_subagent_completion_posts_to_parent_inbox();
    test_subagent_error_posts_to_parent_inbox();
    printf("  ALL PASSED\n");
    return 0;
}
