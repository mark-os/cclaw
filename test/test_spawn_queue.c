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

/* T202: spawn_queue lives in cclaw.db only */
static sqlite3 *setup_daemon_db(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db != NULL);
    return db;
}

/* Helper: insert into spawn_queue directly (daemon does this inline) */
static int64_t sq_insert(sqlite3 *db, const char *agent, int64_t parent_sid,
                         const char *task, int bg, int depth, const char *tc_id) {
    const char *sql =
        "INSERT INTO spawn_queue (parent_agent, parent_session_id, task, background, depth, tool_call_id)"
        " VALUES (?,?,?,?,?,?);";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, parent_sid);
    sqlite3_bind_text(stmt, 3, task, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, bg);
    sqlite3_bind_int(stmt, 5, depth);
    if (tc_id) sqlite3_bind_text(stmt, 6, tc_id, -1, SQLITE_STATIC);
    else sqlite3_bind_null(stmt, 6);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return -1;
    return sqlite3_last_insert_rowid(db);
}

static void test_spawn_queue_insert_and_peek(void) {
    sqlite3 *db = setup_daemon_db();

    int64_t qid = sq_insert(db, "test_agent", 1, "do something", 0, 1, "call_123");
    assert(qid > 0);

    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(reqs != NULL);
    assert(count == 1);
    assert(reqs[0].id == qid);
    assert(reqs[0].parent_session_id == 1);
    assert(strcmp(reqs[0].task, "do something") == 0);
    assert(reqs[0].background == 0);
    assert(reqs[0].depth == 1);
    assert(strcmp(reqs[0].tool_call_id, "call_123") == 0);

    spawn_request_free(reqs, count);
    db_close(db);
    printf("  PASS test_spawn_queue_insert_and_peek\n");
}

static void test_spawn_queue_mark(void) {
    sqlite3 *db = setup_daemon_db();

    int64_t qid = sq_insert(db, "test_agent", 1, "task", 1, 1, NULL);
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
    /* T201: background now returns sentinel (daemon reads args from tool_call) */
    assert(strncmp(r, "AGENT_EXIT_SPAWN:", 17) == 0);
    assert(strstr(r, "background") != NULL);
    free(r);

    /* T201: tool no longer writes to spawn_queue — daemon does after reap */
    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(count == 0);
    assert(reqs == NULL);

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
    assert(strncmp(r, "AGENT_EXIT_SPAWN:", 17) == 0);
    free(r);

    /* T201: tool no longer writes to spawn_queue — daemon does after reap */
    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(count == 0);
    assert(reqs == NULL);

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

/* T282: named spawn — child_agent field populated in spawn_queue */
static void test_spawn_queue_named_agent(void) {
    sqlite3 *db = setup_daemon_db();

    /* Insert with child_agent */
    const char *sql =
        "INSERT INTO spawn_queue (parent_agent, parent_session_id, task, background, depth, tool_call_id, child_agent)"
        " VALUES ('parent','1','do task',0,1,'tc_1','helper');";
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    int count = 0;
    SpawnRequest *reqs = spawn_queue_peek_pending(db, &count);
    assert(reqs != NULL);
    assert(count == 1);
    assert(reqs[0].child_agent != NULL);
    assert(strcmp(reqs[0].child_agent, "helper") == 0);

    /* Mark it so next peek skips it */
    spawn_queue_mark(db, reqs[0].id, "forked", 10);
    spawn_request_free(reqs, count);

    /* Insert without child_agent — should be NULL */
    int64_t qid = sq_insert(db, "parent", 2, "anon task", 0, 1, "tc_2");
    assert(qid > 0);
    reqs = spawn_queue_peek_pending(db, &count);
    assert(count == 1);
    assert(reqs[0].child_agent == NULL);
    spawn_request_free(reqs, count);

    db_close(db);
    printf("  PASS test_spawn_queue_named_agent\n");
}

/* T282: tool accepts name parameter */
static void test_spawn_tool_name_param(void) {
    sqlite3 *db = setup_db();
    int64_t sid = session_create(db, "parent", NULL, -1, 0);
    assert(sid > 0);
    assert(daemon_signal_init() == 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = sid,
                          .daemon_mode = 1, .tool_call_id = "tc_n"};
    char *r = tool_launch_agent_handler(
        "{\"task\":\"run helper\",\"name\":\"helper_agent\"}", &ctx);
    assert(r != NULL);
    /* Should return sentinel (daemon handles spawn) */
    assert(strncmp(r, "AGENT_EXIT_SPAWN:", 17) == 0);
    free(r);

    daemon_signal_close();
    db_close(db);
    printf("  PASS test_spawn_tool_name_param\n");
}

int main(void) {
    printf("test_spawn_queue:\n");
    test_spawn_queue_insert_and_peek();
    test_spawn_queue_mark();
    test_daemon_mode_background();
    test_daemon_mode_blocking();
    test_cli_mode_launch();
    test_depth_limit_daemon_mode();
    test_spawn_queue_named_agent();
    test_spawn_tool_name_param();
    printf("All spawn queue tests passed.\n");
    return 0;
}
