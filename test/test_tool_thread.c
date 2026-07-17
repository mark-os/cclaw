/* test_tool_thread.c — the EXEC_THREAD completion-notification path (r4 F19).
 * A lost notification doesn't error, it silently stalls a session in
 * tool_running forever — so the contract under test is: spawn → thread runs
 * the handler on its OWN db handle → result entry written + tool_call done →
 * session_id arrives on the notify pipe. */
#define _POSIX_C_SOURCE 200809L
#include "tool_thread.h"
#include "db.h"
#include "db_response.h"
#include "test_util.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB "/tmp/cclaw_test_tool_thread.db"

/* Handler run on the tool thread. Records the db handle it was given so the
 * test can assert it's NOT the poll thread's handle. */
static sqlite3 *g_seen_db;
static char *ok_run(sqlite3 *db, const char *agent_name,
                    int64_t session_id, const char *args) {
    (void)agent_name; (void)session_id; (void)args;
    g_seen_db = db;
    return strdup("thread says hi");
}

static char *null_run(sqlite3 *db, const char *agent_name,
                      int64_t session_id, const char *args) {
    (void)db; (void)agent_name; (void)session_id; (void)args;
    return NULL;   /* must surface as an error result, not a lost turn */
}

/* Insert a session + assistant tool_call entry + tool_calls workflow row.
 * Returns session id; entry id and call id via out params. */
static int64_t seed_call(sqlite3 *db, const char *call_id, int64_t *entry_id) {
    int64_t sid = session_create(db, "tt", NULL, -1, 0);
    assert(sid > 0);
    int64_t eid = entry_append_typed(db, sid, 1, "tool_call", 0,
        "{}", call_id, "list_memory", 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(eid > 0);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO tool_calls(session_id, entry_id, call_id, name, status)"
        " VALUES(?, ?, ?, 'list_memory', 'running');", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, sid);
    sqlite3_bind_int64(st, 2, eid);
    sqlite3_bind_text(st, 3, call_id, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    if (entry_id) *entry_id = eid;
    return sid;
}

/* Wait (bounded) for one completion notification; returns the session id. */
static int64_t wait_notify(void) {
    struct pollfd pfd = { .fd = tool_thread_fd(), .events = POLLIN };
    int pr = poll(&pfd, 1, 10000);
    assert(pr == 1);
    int64_t sid = -1;
    assert(tool_thread_read(&sid) == 0);
    return sid;
}

static void job_init(ToolThreadJob *job, int64_t sid, int64_t eid,
                     const char *call_id,
                     char *(*run)(sqlite3 *, const char *, int64_t, const char *)) {
    memset(job, 0, sizeof(*job));
    job->session_id = sid;
    job->entry_id = eid;
    snprintf(job->tool_call_id, sizeof(job->tool_call_id), "%s", call_id);
    snprintf(job->tool_name, sizeof(job->tool_name), "list_memory");
    snprintf(job->agent_name, sizeof(job->agent_name), "default");
    job->args = strdup("{}");
    job->run = run;
}

static void test_spawn_before_start_fails(void) {
    ToolThreadJob job;
    job_init(&job, 1, 1, "c0", ok_run);
    assert(tool_thread_spawn(&job) == -1);   /* also frees job.args */
    printf("  PASS test_spawn_before_start_fails\n");
}

static void test_completion_notifies_and_writes(void) {
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    int64_t eid = 0;
    int64_t sid = seed_call(db, "c1", &eid);

    ToolThreadJob job;
    job_init(&job, sid, eid, "c1", ok_run);
    assert(tool_thread_spawn(&job) == 0);

    assert(wait_notify() == sid);

    /* The thread used its own handle, not ours. */
    assert(g_seen_db != NULL && g_seen_db != db);

    /* tool_call completed with a result entry that carries the output. */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT tc.status, e.content FROM tool_calls tc"
        " JOIN entries e ON e.id = tc.result_entry_id"
        " WHERE tc.call_id='c1';", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *status = (const char *)sqlite3_column_text(st, 0);
    const char *content = (const char *)sqlite3_column_text(st, 1);
    assert(status && strcmp(status, "done") == 0);
    assert(content && strstr(content, "thread says hi"));
    sqlite3_finalize(st);

    db_close(db);
    printf("  PASS test_completion_notifies_and_writes\n");
}

static void test_null_result_still_completes(void) {
    /* The silent-failure shape F19 names: if a handler returns NULL and the
     * thread didn't write an error result + notify, the session would stall
     * in tool_running with nothing in the log. */
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    int64_t eid = 0;
    int64_t sid = seed_call(db, "c2", &eid);

    ToolThreadJob job;
    job_init(&job, sid, eid, "c2", null_run);
    assert(tool_thread_spawn(&job) == 0);

    assert(wait_notify() == sid);

    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT tc.status, e.content FROM tool_calls tc"
        " JOIN entries e ON e.id = tc.result_entry_id"
        " WHERE tc.call_id='c2';", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *status = (const char *)sqlite3_column_text(st, 0);
    const char *content = (const char *)sqlite3_column_text(st, 1);
    assert(status && strcmp(status, "done") == 0);
    assert(content && strncmp(content, "error:", 6) == 0);
    sqlite3_finalize(st);

    db_close(db);
    printf("  PASS test_null_result_still_completes\n");
}

static void test_stop_drains_in_flight(void) {
    /* The drain barrier must not return before in-flight threads finish
     * (a detached thread racing teardown was the rare exit SIGSEGV). */
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    int64_t eid = 0;
    int64_t sid = seed_call(db, "c3", &eid);

    ToolThreadJob job;
    job_init(&job, sid, eid, "c3", ok_run);
    assert(tool_thread_spawn(&job) == 0);
    tool_thread_stop();   /* blocks until the thread exits */

    /* Post-stop, the work is durably in the DB even if the notify pipe is
     * gone. */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT status FROM tool_calls WHERE call_id='c3';", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *status = (const char *)sqlite3_column_text(st, 0);
    assert(status && strcmp(status, "done") == 0);
    sqlite3_finalize(st);

    /* Spawning after stop must refuse, not crash. */
    job_init(&job, sid, eid, "c4", ok_run);
    assert(tool_thread_spawn(&job) == -1);

    db_close(db);
    printf("  PASS test_stop_drains_in_flight\n");
}

int main(void) {
    TEST_INIT();
    alarm(15);
    printf("test_tool_thread:\n");
    test_db_clean(TEST_DB);

    test_spawn_before_start_fails();

    assert(tool_thread_start(TEST_DB, 0) == 0);
    test_completion_notifies_and_writes();
    test_null_result_still_completes();
    test_stop_drains_in_flight();

    test_db_clean(TEST_DB);
    printf("All tool_thread tests passed.\n");
    return 0;
}
