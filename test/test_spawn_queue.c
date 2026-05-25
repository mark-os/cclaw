/* T88: Daemon spawn queue — agent posts request, daemon picks up + forks */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "db.h"
#include "daemon.h"
#include "tool_agent.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sqlite3 *setup_db(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db != NULL);
    return db;
}

static void test_spawn_queue_insert_and_peek(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create(db, "parent", NULL, -1, 0);
    assert(sid > 0);

    int64_t qid = spawn_queue_insert(db, sid, "do something", 0, 1, "call_123");
    assert(qid > 0);

    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(reqs != NULL);
    assert(count == 1);
    assert(reqs[0].id == qid);
    assert(reqs[0].parent_session_id == sid);
    assert(strcmp(reqs[0].task, "do something") == 0);
    assert(reqs[0].background == 0);
    assert(reqs[0].depth == 1);
    assert(strcmp(reqs[0].tool_call_id, "call_123") == 0);

    spawn_request_free(reqs, count);
    db_close(db);
    printf("  PASS test_spawn_queue_insert_and_peek\n");
}

static void test_spawn_queue_mark(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create(db, "parent", NULL, -1, 0);
    int64_t qid = spawn_queue_insert(db, sid, "task", 1, 1, NULL);
    assert(qid > 0);

    /* Mark as forked */
    assert(spawn_queue_mark(db, qid, "forked", 99) == 0);

    /* Should no longer appear in pending */
    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(reqs == NULL);
    assert(count == 0);

    db_close(db);
    printf("  PASS test_spawn_queue_mark\n");
}

static void test_daemon_mode_background(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create(db, "parent", NULL, -1, 0);
    assert(sid > 0);

    /* Init signal pipe so daemon_signal_session doesn't fail */
    assert(daemon_signal_init() == 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = sid,                        .daemon_mode = 1, .tool_call_id = NULL};
    char *r = tool_launch_agent_handler("{\"task\":\"bg task\",\"background\":true}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "spawn request queued") != NULL);
    assert(strstr(r, "background") != NULL);
    free(r);

    /* Verify it's in the queue */
    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(count == 1);
    assert(reqs[0].background == 1);
    assert(strcmp(reqs[0].task, "bg task") == 0);
    spawn_request_free(reqs, count);

    daemon_signal_close();
    db_close(db);
    printf("  PASS test_daemon_mode_background\n");
}

static void test_daemon_mode_blocking(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create(db, "parent", NULL, -1, 0);
    assert(sid > 0);

    assert(daemon_signal_init() == 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = sid,                        .daemon_mode = 1, .tool_call_id = "tc_001"};
    char *r = tool_launch_agent_handler("{\"task\":\"blocking task\"}", &ctx);
    assert(r != NULL);
    /* Blocking mode returns sentinel */
    assert(strstr(r, "SPAWN_BLOCKING:") != NULL);
    free(r);

    /* Verify queue entry */
    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(count == 1);
    assert(reqs[0].background == 0);
    assert(strcmp(reqs[0].tool_call_id, "tc_001") == 0);
    spawn_request_free(reqs, count);

    daemon_signal_close();
    db_close(db);
    printf("  PASS test_daemon_mode_blocking\n");
}

static void test_cli_mode_launch(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create(db, "parent", NULL, -1, 0);
    assert(sid > 0);

    /* daemon_mode = 0 → CLI mode, uses spawn_queue + polling */
    AgentLaunchCtx ctx = {.db = db, .session_id = sid,                        .self_path = "/bin/true", .daemon_mode = 0, .tool_call_id = NULL};
    char *r = tool_launch_agent_handler("{\"task\":\"cli task\",\"background\":true}", &ctx);
    assert(r != NULL);
    /* CLI background mode returns "launched agent" */
    assert(strstr(r, "launched agent") != NULL);
    free(r);

    db_close(db);
    printf("  PASS test_cli_mode_launch\n");
}

static void test_depth_limit_daemon_mode(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create(db, "deep", NULL, 1, AGENT_MAX_DEPTH);

    assert(daemon_signal_init() == 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = sid,
                       .daemon_mode = 1, .tool_call_id = NULL};
    char *r = tool_launch_agent_handler("{\"task\":\"too deep\"}", &ctx);
    assert(strstr(r, "max agent depth") != NULL);
    free(r);

    /* Nothing in queue */
    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(reqs == NULL);

    daemon_signal_close();
    db_close(db);
    printf("  PASS test_depth_limit_daemon_mode\n");
}

int main(void) {
    printf("test_spawn_queue:\n");
    test_spawn_queue_insert_and_peek();
    test_spawn_queue_mark();
    test_daemon_mode_background();
    test_daemon_mode_blocking();
    test_cli_mode_launch();
    test_depth_limit_daemon_mode();
    printf("All spawn queue tests passed.\n");
    return 0;
}
