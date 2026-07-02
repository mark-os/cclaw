#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tool_agent.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static sqlite3 *setup_db(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db != NULL);
    /* launch_agent refuses names without an agents row */
    sqlite3_exec(db, "INSERT INTO agents(name) VALUES('default')", NULL, NULL, NULL);
    return db;
}

static void test_invalid_json(void) {
    AgentLaunchCtx ctx = {.db = NULL, .session_id = 1};
    char *r = tool_launch_agent_handler("not json", &ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_invalid_json\n");
}

static void test_missing_task(void) {
    sqlite3 *db = setup_db();
    AgentLaunchCtx ctx = {.db = db, .session_id = 1};
    char *r = tool_launch_agent_handler("{\"foo\":\"bar\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_missing_task\n");
}

static void test_depth_limit(void) {
    sqlite3 *db = setup_db();
    /* Create a session at max depth */
    int64_t sid = session_create(db, "deep", NULL, 1, AGENT_MAX_DEPTH);
    assert(sid > 0);
    AgentLaunchCtx ctx = {.db = db, .session_id = sid};
    char *r = tool_launch_agent_handler("{\"task\":\"hello\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "max agent depth") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_depth_limit\n");
}

static void test_per_parent_limit(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    /* Create 3 running child sessions for this parent */
    for (int i = 0; i < AGENT_MAX_PER_PARENT; i++) {
        int64_t child_sid = session_create(db, "child", NULL, parent_sid, 1);
        assert(child_sid > 0);
        /* Mark as running */
        session_set_state(db, child_sid, "llm_running");
    }

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    char *r = tool_launch_agent_handler("{\"task\":\"one more\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "max sub-agents per parent") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_per_parent_limit\n");
}

static void test_system_wide_limit(void) {
    sqlite3 *db = setup_db();

    /* Create 10 running child sessions across different parents */
    for (int i = 0; i < AGENT_MAX_TOTAL; i++) {
        int64_t psid = session_create(db, "p", NULL, -1, 0);
        int64_t csid = session_create(db, "c", NULL, psid, 1);
        session_set_state(db, csid, "llm_running");
    }

    int64_t my_sid = session_create(db, "me", "default", -1, 0);
    AgentLaunchCtx ctx = {.db = db, .session_id = my_sid};
    char *r = tool_launch_agent_handler("{\"task\":\"overflow\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "max system-wide") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_system_wide_limit\n");
}

static void test_spawn_background(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    char *r = tool_launch_agent_handler("{\"task\":\"test task\",\"background\":true}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "agent delegated") != NULL);

    free(r);
    db_close(db);
    printf("  PASS test_spawn_background\n");
}

static void test_spawn_blocking(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    /* Blocking launch returns NULL (no inline result — the child writes the
     * tool result on completion) and records the parent's tool_call_id on the
     * child so completion can route back. The handler does NOT change parent
     * state: the dispatcher marks the tool_call 'running' and the parent stays
     * in tool_running, so sibling launch_agent calls keep dispatching. */
    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid,
                          .current_tool_call_id = "call_123"};
    session_set_state(db, parent_sid, "llm_running");
    session_set_state(db, parent_sid, "tool_running");
    char *r = tool_launch_agent_handler("{\"task\":\"blocking task\"}", &ctx);
    assert(r == NULL); /* NULL = async, real result comes on child completion */

    /* Parent state is unchanged by the handler (still tool_running) */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, parent_sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "tool_running") == 0);
    sqlite3_finalize(stmt);

    /* The child carries the parent's tool_call_id for result routing */
    sqlite3_prepare_v2(db,
        "SELECT parent_tool_call_id FROM sessions WHERE parent_session_id=?"
        " ORDER BY id DESC LIMIT 1", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, parent_sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "call_123") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  PASS test_spawn_blocking\n");
}

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    AgentLaunchCtx ctx = {.db = NULL, .session_id = 1};
    int rc = tool_launch_agent_register(&reg, &ctx);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "launch_agent");
    assert(e != NULL);
    assert(e->handler == tool_launch_agent_handler);
    rc = tool_check_session_register(&reg, &ctx);
    assert(rc == 0);
    e = tools_lookup(&reg, "check_session");
    assert(e != NULL);
    assert(e->handler == tool_check_session_handler);
    tools_free(&reg);
    printf("  PASS test_register\n");
}

static void test_unknown_agent_rejected(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    /* Trust policy comes from the agents row; an unregistered name must be
     * refused, not spawned (it would run with no row to derive policy from) */
    char *r = tool_launch_agent_handler(
        "{\"task\":\"x\",\"name\":\"x-nonexistent\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "unknown agent") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_unknown_agent_rejected\n");
}

static void test_check_agent_not_found(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    char *r = tool_check_session_handler("{\"session_id\":9999}", &ctx);
    assert(strstr(r, "not found") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_check_agent_not_found\n");
}

static void test_check_agent_idle_with_result(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    int64_t child_sid = session_create(db, "child", NULL, parent_sid, 1);

    /* Add an assistant entry to child session */
    Message msg = {.role = ROLE_ASSISTANT, .content = "the answer is 42"};
    entry_append_with_turn(db, child_sid, &msg, 1);

    char args[64];
    snprintf(args, sizeof(args), "{\"session_id\":%lld}", (long long)child_sid);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    char *r = tool_check_session_handler(args, &ctx);
    assert(strstr(r, "state: idle") != NULL);
    assert(strstr(r, "result: the answer is 42") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_check_agent_idle_with_result\n");
}

static void test_session_count_children(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);

    /* No children yet */
    assert(session_count_children(db, parent_sid) == 0);

    /* Create child, mark running */
    int64_t c1 = session_create(db, "c1", NULL, parent_sid, 1);
    session_set_state(db, c1, "llm_running");
    assert(session_count_children(db, parent_sid) == 1);

    /* Create another, mark running */
    int64_t c2 = session_create(db, "c2", NULL, parent_sid, 1);
    session_set_state(db, c2, "llm_running");
    assert(session_count_children(db, parent_sid) == 2);

    /* Release one — should drop count */
    session_set_state(db, c1, "idle");
    assert(session_count_children(db, parent_sid) == 1);

    db_close(db);
    printf("  PASS test_session_count_children\n");
}

/* --- Filter resolution tests for launch_agent --- */

/* Helper: get the newest child session's tool_filter for a given parent */
static char *child_tool_filter(sqlite3 *db, int64_t parent_sid) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT tool_filter FROM sessions WHERE parent_session_id=?"
                      " ORDER BY id DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, parent_sid);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = strdup(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

/* Helper: get the newest child session's agent_name for a given parent */
static char *child_agent_name(sqlite3 *db, int64_t parent_sid) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT agent_name FROM sessions WHERE parent_session_id=?"
                      " ORDER BY id DESC LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(stmt, 1, parent_sid);
    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *val = (const char *)sqlite3_column_text(stmt, 0);
        if (val) result = strdup(val);
    }
    sqlite3_finalize(stmt);
    return result;
}

/* Helper: count child sessions for a parent */
static int child_count(sqlite3 *db, int64_t parent_sid) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT COUNT(*) FROM sessions WHERE parent_session_id=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(stmt, 1, parent_sid);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

static void test_filter_explicit_tools(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    /* Self-spawn (no "name") with explicit "tools" */
    char *r = tool_launch_agent_handler(
        "{\"task\":\"work\",\"background\":true,\"tools\":[\"file_read\"]}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "agent delegated") != NULL);
    free(r);

    /* Child session's tool_filter should be the explicit array */
    char *filter = child_tool_filter(db, parent_sid);
    assert(filter != NULL);
    assert(strcmp(filter, "[\"file_read\"]") == 0);
    free(filter);

    db_close(db);
    printf("  PASS test_filter_explicit_tools\n");
}

static void test_filter_kv_worker_tools(void) {
    sqlite3 *db = setup_db();
    /* Set worker_tools in kv config */
    db_kv_set(db, "worker_tools", "[\"js_eval\"]");

    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    /* Self-spawn without "tools" — should pick up kv worker_tools */
    char *r = tool_launch_agent_handler(
        "{\"task\":\"work\",\"background\":true}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "agent delegated") != NULL);
    free(r);

    char *filter = child_tool_filter(db, parent_sid);
    assert(filter != NULL);
    assert(strcmp(filter, "[\"js_eval\"]") == 0);
    free(filter);

    db_close(db);
    printf("  PASS test_filter_kv_worker_tools\n");
}

static void test_filter_neither(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    /* Self-spawn with neither explicit tools nor kv worker_tools → NULL filter */
    char *r = tool_launch_agent_handler(
        "{\"task\":\"work\",\"background\":true}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "agent delegated") != NULL);
    free(r);

    char *filter = child_tool_filter(db, parent_sid);
    assert(filter == NULL); /* NULL = unrestricted */

    db_close(db);
    printf("  PASS test_filter_neither\n");
}

static void test_tools_with_name_rejected(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    int before = child_count(db, parent_sid);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    /* Passing "tools" with an explicit "name" → error */
    char *r = tool_launch_agent_handler(
        "{\"task\":\"x\",\"name\":\"default\",\"tools\":[\"file_read\"],\"background\":true}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "only valid for self-spawn") != NULL);
    free(r);

    /* No child session created */
    int after = child_count(db, parent_sid);
    assert(after == before);

    db_close(db);
    printf("  PASS test_tools_with_name_rejected\n");
}

static void test_self_spawn_inherits_agent_name(void) {
    sqlite3 *db = setup_db();
    /* Parent session owned by 'default' agent */
    int64_t parent_sid = session_create(db, "parent", "default", -1, 0);
    assert(parent_sid > 0);

    AgentLaunchCtx ctx = {.db = db, .session_id = parent_sid};
    char *r = tool_launch_agent_handler(
        "{\"task\":\"selfwork\",\"background\":true}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "agent delegated") != NULL);
    free(r);

    /* Child session's agent_name must match the parent's: 'default' */
    char *name = child_agent_name(db, parent_sid);
    assert(name != NULL);
    assert(strcmp(name, "default") == 0);
    free(name);

    db_close(db);
    printf("  PASS test_self_spawn_inherits_agent_name\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_tool_agent:\n");
    test_invalid_json();
    test_missing_task();
    test_depth_limit();
    test_per_parent_limit();
    test_system_wide_limit();
    test_spawn_background();
    test_spawn_blocking();
    test_register();
    test_unknown_agent_rejected();
    test_check_agent_not_found();
    test_check_agent_idle_with_result();
    test_session_count_children();
    test_filter_explicit_tools();
    test_filter_kv_worker_tools();
    test_filter_neither();
    test_tools_with_name_rejected();
    test_self_spawn_inherits_agent_name();
    printf("All sub-agent tool tests passed.\n");
    return 0;
}
