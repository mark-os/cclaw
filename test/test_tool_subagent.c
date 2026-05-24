#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "tool_subagent.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

static sqlite3 *setup_db(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db != NULL);
    return db;
}

static void test_invalid_json(void) {
    SubAgentCtx ctx = {.db = NULL, .session_id = 1, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_spawn_agent_handler("not json", &ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_invalid_json\n");
}

static void test_missing_task(void) {
    sqlite3 *db = setup_db();
    SubAgentCtx ctx = {.db = db, .session_id = 1, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_spawn_agent_handler("{\"foo\":\"bar\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_missing_task\n");
}

static void test_depth_limit(void) {
    sqlite3 *db = setup_db();
    SubAgentCtx ctx = {.db = db, .session_id = 1, .depth = 2, .self_path = "/bin/true"};
    char *r = tool_spawn_agent_handler("{\"task\":\"hello\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "max sub-agent depth") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_depth_limit\n");
}

static void test_per_parent_limit(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", NULL);
    assert(parent_sid > 0);

    /* Create 3 running sub-agents for this parent */
    for (int i = 0; i < SUBAGENT_MAX_PER_PARENT; i++) {
        int64_t child_sid = session_create(db, "child", NULL);
        int64_t aid = subagent_create(db, parent_sid, child_sid, 9999 + i, 1, "task");
        assert(aid > 0);
    }

    SubAgentCtx ctx = {.db = db, .session_id = parent_sid, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_spawn_agent_handler("{\"task\":\"one more\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "max concurrent sub-agents per parent") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_per_parent_limit\n");
}

static void test_system_wide_limit(void) {
    sqlite3 *db = setup_db();

    /* Create 10 running sub-agents across different parents */
    for (int i = 0; i < SUBAGENT_MAX_TOTAL; i++) {
        int64_t psid = session_create(db, "p", NULL);
        int64_t csid = session_create(db, "c", NULL);
        int64_t aid = subagent_create(db, psid, csid, 8000 + i, 1, "task");
        assert(aid > 0);
    }

    int64_t my_sid = session_create(db, "me", NULL);
    SubAgentCtx ctx = {.db = db, .session_id = my_sid, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_spawn_agent_handler("{\"task\":\"overflow\"}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "max system-wide") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_system_wide_limit\n");
}

static void test_spawn_success(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", NULL);
    assert(parent_sid > 0);

    /* Use /bin/true as self_path — background mode returns immediately */
    SubAgentCtx ctx = {.db = db, .session_id = parent_sid, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_spawn_agent_handler("{\"task\":\"test task\",\"background\":true}", &ctx);
    assert(r != NULL);
    assert(strstr(r, "spawned sub-agent") != NULL);
    assert(strstr(r, "id=") != NULL);

    /* Verify DB record */
    int count = subagent_count_by_parent(db, parent_sid);
    assert(count == 1);

    int total = subagent_count_total(db);
    assert(total == 1);

    free(r);
    db_close(db);
    printf("  PASS test_spawn_success\n");
}

static void test_spawn_blocking(void) {
    sqlite3 *db = setup_db();
    int64_t parent_sid = session_create(db, "parent", NULL);
    assert(parent_sid > 0);

    /* /bin/true exits 0 but won't call subagent_finish — tests the "no output" path */
    SubAgentCtx ctx = {.db = db, .session_id = parent_sid, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_spawn_agent_handler("{\"task\":\"blocking task\"}", &ctx);
    assert(r != NULL);
    /* Should NOT contain "spawned sub-agent" (that's background mode) */
    assert(strstr(r, "spawned sub-agent") == NULL);
    /* /bin/true exits 0 but doesn't write result, so we get the fallback */
    assert(strstr(r, "sub-agent") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_spawn_blocking\n");
}

static void test_subagent_finish(void) {
    sqlite3 *db = setup_db();
    int64_t psid = session_create(db, "p", NULL);
    int64_t csid = session_create(db, "c", NULL);
    int64_t aid = subagent_create(db, psid, csid, 1234, 1, "do stuff");
    assert(aid > 0);

    assert(subagent_count_total(db) == 1);

    int rc = subagent_finish(db, aid, "done", "result text");
    assert(rc == 0);

    /* After finishing, should not count as running */
    assert(subagent_count_total(db) == 0);

    SubAgentInfo *info = subagent_get(db, aid);
    assert(info != NULL);
    assert(strcmp(info->status, "done") == 0);
    assert(strcmp(info->result, "result text") == 0);
    assert(strcmp(info->task, "do stuff") == 0);
    subagent_info_free(info);

    db_close(db);
    printf("  PASS test_subagent_finish\n");
}

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    SubAgentCtx ctx = {.db = NULL, .session_id = 1, .depth = 0, .self_path = "/bin/true"};
    int rc = tool_spawn_agent_register(&reg, &ctx);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "spawn_agent");
    assert(e != NULL);
    assert(e->handler == tool_spawn_agent_handler);
    rc = tool_check_agent_register(&reg, &ctx);
    assert(rc == 0);
    e = tools_lookup(&reg, "check_agent");
    assert(e != NULL);
    assert(e->handler == tool_check_agent_handler);
    tools_free(&reg);
    printf("  PASS test_register\n");
}

/* --- check_agent tests (V13) --- */

static void test_check_agent_invalid_json(void) {
    SubAgentCtx ctx = {.db = NULL, .session_id = 1, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_check_agent_handler("bad", &ctx);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_check_agent_invalid_json\n");
}

static void test_check_agent_not_found(void) {
    sqlite3 *db = setup_db();
    SubAgentCtx ctx = {.db = db, .session_id = 1, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_check_agent_handler("{\"agent_id\":9999}", &ctx);
    assert(strstr(r, "not found") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_check_agent_not_found\n");
}

static void test_check_agent_running(void) {
    sqlite3 *db = setup_db();
    int64_t psid = session_create(db, "p", NULL);
    int64_t csid = session_create(db, "c", NULL);
    int64_t aid = subagent_create(db, psid, csid, 1234, 1, "my task");
    assert(aid > 0);

    char args[64];
    snprintf(args, sizeof(args), "{\"agent_id\":%lld}", (long long)aid);

    SubAgentCtx ctx = {.db = db, .session_id = psid, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_check_agent_handler(args, &ctx);
    assert(strstr(r, "running") != NULL);
    assert(strstr(r, "my task") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_check_agent_running\n");
}

static void test_check_agent_done(void) {
    sqlite3 *db = setup_db();
    int64_t psid = session_create(db, "p", NULL);
    int64_t csid = session_create(db, "c", NULL);
    int64_t aid = subagent_create(db, psid, csid, 1234, 1, "do stuff");
    assert(aid > 0);
    subagent_finish(db, aid, "done", "the answer is 42");

    char args[64];
    snprintf(args, sizeof(args), "{\"agent_id\":%lld}", (long long)aid);

    SubAgentCtx ctx = {.db = db, .session_id = psid, .depth = 0, .self_path = "/bin/true"};
    char *r = tool_check_agent_handler(args, &ctx);
    assert(strstr(r, "done") != NULL);
    assert(strstr(r, "the answer is 42") != NULL);
    assert(strstr(r, "do stuff") != NULL);
    free(r);
    db_close(db);
    printf("  PASS test_check_agent_done\n");
}

/* --- T39: sub-agent lifecycle tests --- */

static void test_reap_crashed(void) {
    sqlite3 *db = setup_db();
    int64_t psid = session_create(db, "p", NULL);
    int64_t csid = session_create(db, "c", NULL);
    /* Use PID 1 which we can't waitpid on (ECHILD) — simulates gone process */
    int64_t aid = subagent_create(db, psid, csid, 99999, 1, "crash task");
    assert(aid > 0);
    assert(subagent_count_total(db) == 1);

    /* Reap should detect the process is gone and mark crashed */
    int reaped = subagent_reap(db);
    assert(reaped == 1);

    /* Verify status updated */
    SubAgentInfo *info = subagent_get(db, aid);
    assert(info != NULL);
    assert(strcmp(info->status, "crashed") == 0);
    assert(info->result != NULL);
    subagent_info_free(info);

    /* No longer counts as running */
    assert(subagent_count_total(db) == 0);

    db_close(db);
    printf("  PASS test_reap_crashed\n");
}

static void test_reap_exited_ok(void) {
    sqlite3 *db = setup_db();
    int64_t psid = session_create(db, "p", NULL);
    int64_t csid = session_create(db, "c", NULL);

    /* Fork a child that exits immediately with 0 */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) _exit(0);

    /* Small delay to let child exit */
    usleep(50000);

    int64_t aid = subagent_create(db, psid, csid, pid, 1, "ok task");
    assert(aid > 0);

    int reaped = subagent_reap(db);
    assert(reaped == 1);

    SubAgentInfo *info = subagent_get(db, aid);
    assert(info != NULL);
    assert(strcmp(info->status, "done") == 0);
    subagent_info_free(info);

    db_close(db);
    printf("  PASS test_reap_exited_ok\n");
}

static void test_reap_exited_error(void) {
    sqlite3 *db = setup_db();
    int64_t psid = session_create(db, "p", NULL);
    int64_t csid = session_create(db, "c", NULL);

    /* Fork a child that exits with non-zero */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) _exit(1);

    usleep(50000);

    int64_t aid = subagent_create(db, psid, csid, pid, 1, "fail task");
    assert(aid > 0);

    int reaped = subagent_reap(db);
    assert(reaped == 1);

    SubAgentInfo *info = subagent_get(db, aid);
    assert(info != NULL);
    assert(strcmp(info->status, "crashed") == 0);
    subagent_info_free(info);

    db_close(db);
    printf("  PASS test_reap_exited_error\n");
}

static void test_reap_still_running(void) {
    sqlite3 *db = setup_db();
    int64_t psid = session_create(db, "p", NULL);
    int64_t csid = session_create(db, "c", NULL);

    /* Fork a child that sleeps */
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) { sleep(60); _exit(0); }

    int64_t aid = subagent_create(db, psid, csid, pid, 1, "long task");
    assert(aid > 0);

    /* Reap should not touch it */
    int reaped = subagent_reap(db);
    assert(reaped == 0);
    assert(subagent_count_total(db) == 1);

    SubAgentInfo *info = subagent_get(db, aid);
    assert(info != NULL);
    assert(strcmp(info->status, "running") == 0);
    subagent_info_free(info);

    /* Cleanup */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    db_close(db);
    printf("  PASS test_reap_still_running\n");
}

int main(void) {
    printf("test_tool_subagent:\n");
    test_invalid_json();
    test_missing_task();
    test_depth_limit();
    test_per_parent_limit();
    test_system_wide_limit();
    test_spawn_success();
    test_spawn_blocking();
    test_subagent_finish();
    test_register();
    test_check_agent_invalid_json();
    test_check_agent_not_found();
    test_check_agent_running();
    test_check_agent_done();
    test_reap_crashed();
    test_reap_exited_ok();
    test_reap_exited_error();
    test_reap_still_running();
    printf("All sub-agent tool tests passed.\n");
    return 0;
}
