/* Test tool_check_approval_handler: status, rerequest of decided/pending/bogus */
#define _POSIX_C_SOURCE 200809L
#include "approval.h"
#include "db.h"
#include "tool_agent.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_tool_check_approval.db"

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

static void test_status_lists_approvals(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);

    int64_t id1 = approval_create(db, sid, "call_1", "shell_exec", "rm -rf", "{}", "rerun");
    int64_t id2 = approval_create(db, sid, "call_2", "web_fetch", "curl", "{}", "rerun");
    assert(id1 > 0 && id2 > 0);

    AgentLaunchCtx ctx = { .db = db, .session_id = sid, .current_tool_call_id = "chk" };
    char *res = tool_check_approval_handler("{\"action\":\"status\"}", &ctx);
    assert(res != NULL);
    assert(strncmp(res, "error:", 6) != 0);
    assert(strlen(res) > 0);
    free(res);

    db_close(db);
    clean_db();
    printf("  PASS test_status_lists_approvals\n");
}

static void test_rerequest_decided(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);

    int64_t id = approval_create(db, sid, "call_d", "shell_exec", "ls", "{}", "rerun");
    assert(id > 0);

    /* Deny it so it becomes terminal */
    Approval *resolved = approval_resolve(db, id, 0, "test");
    assert(resolved != NULL);
    approval_free(resolved);

    char args[128];
    snprintf(args, sizeof(args), "{\"action\":\"rerequest\",\"approval_id\":%lld}", (long long)id);

    AgentLaunchCtx ctx = { .db = db, .session_id = sid, .current_tool_call_id = "chk" };
    char *res = tool_check_approval_handler(args, &ctx);
    assert(res != NULL);
    assert(strncmp(res, "error:", 6) != 0);

    /* A new pending approval should exist */
    Approval *pending = approval_get_pending(db, sid);
    assert(pending != NULL);
    approval_free(pending);
    free(res);

    db_close(db);
    clean_db();
    printf("  PASS test_rerequest_decided\n");
}

static void test_rerequest_pending_errors(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);

    int64_t id = approval_create(db, sid, "call_p", "shell_exec", "ls", "{}", "rerun");
    assert(id > 0);

    char args[128];
    snprintf(args, sizeof(args), "{\"action\":\"rerequest\",\"approval_id\":%lld}", (long long)id);

    AgentLaunchCtx ctx = { .db = db, .session_id = sid, .current_tool_call_id = "chk" };
    char *res = tool_check_approval_handler(args, &ctx);
    assert(res != NULL);
    assert(strncmp(res, "error:", 6) == 0);
    free(res);

    db_close(db);
    clean_db();
    printf("  PASS test_rerequest_pending_errors\n");
}

static void test_rerequest_bogus_id_errors(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);

    AgentLaunchCtx ctx = { .db = db, .session_id = sid, .current_tool_call_id = "chk" };
    char *res = tool_check_approval_handler("{\"action\":\"rerequest\",\"approval_id\":99999}", &ctx);
    assert(res != NULL);
    assert(strncmp(res, "error:", 6) == 0);
    free(res);

    db_close(db);
    clean_db();
    printf("  PASS test_rerequest_bogus_id_errors\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_tool_check_approval:\n");
    test_status_lists_approvals();
    test_rerequest_decided();
    test_rerequest_pending_errors();
    test_rerequest_bogus_id_errors();
    printf("all tool_check_approval tests passed\n");
    return 0;
}
