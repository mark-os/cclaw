/* T89: Daemon forks agent on inbox signal, reaps on exit, delivers response */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "daemon.h"
#include "db.h"
#include "shutdown.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_daemon.sqlite";

/* ── T82: Signal pipe tests ─────────────────────────────────────── */

static void test_signal_pipe_init_and_write(void) {
    assert(daemon_signal_init() == 0);
    int fd = daemon_signal_fd();
    assert(fd >= 0);

    int64_t sid = 42;
    assert(daemon_signal_session(sid) == 0);

    /* Read it back */
    int64_t got;
    ssize_t n = read(fd, &got, sizeof(got));
    assert(n == sizeof(got));
    assert(got == 42);

    daemon_signal_close();
    printf("  PASS test_signal_pipe_init_and_write\n");
}

static void test_signal_pipe_multiple(void) {
    assert(daemon_signal_init() == 0);
    int fd = daemon_signal_fd();

    daemon_signal_session(1);
    daemon_signal_session(2);
    daemon_signal_session(3);

    int64_t got;
    read(fd, &got, sizeof(got)); assert(got == 1);
    read(fd, &got, sizeof(got)); assert(got == 2);
    read(fd, &got, sizeof(got)); assert(got == 3);

    daemon_signal_close();
    printf("  PASS test_signal_pipe_multiple\n");
}

/* ── T87: Child tracking tests ──────────────────────────────────── */

static void test_child_tracking(void) {
    /* daemon_child_session returns -1 for unknown pid */
    assert(daemon_child_session(99999) == -1);
    printf("  PASS test_child_tracking\n");
}

/* ── T86: last_route tracking ───────────────────────────────────── */

static void test_last_route(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "route_test", NULL);
    assert(sid > 0);

    /* Initially NULL */
    char *route = session_get_last_route(db, sid);
    assert(route == NULL);

    /* Set and read back */
    assert(session_set_last_route(db, sid, "telegram") == 0);
    route = session_get_last_route(db, sid);
    assert(route && strcmp(route, "telegram") == 0);
    free(route);

    /* Update */
    assert(session_set_last_route(db, sid, "cli") == 0);
    route = session_get_last_route(db, sid);
    assert(route && strcmp(route, "cli") == 0);
    free(route);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_last_route\n");
}

/* ── T81/T89: Daemon fork/reap integration ──────────────────────── */

static void *daemon_thread(void *arg) {
    void **args = (void **)arg;
    Config *cfg = (Config *)args[0];
    sqlite3 *db = (sqlite3 *)args[1];
    daemon_run(cfg, db);
    return NULL;
}

static void test_daemon_fork_reap(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    /* Create a session with inbox item */
    int64_t sid = session_create(db, "daemon_test", NULL);
    assert(sid > 0);

    /* Insert a system prompt so agent has context */
    Message sys_msg = {.role = ROLE_SYSTEM, .content = "Reply with exactly: DAEMON_OK"};
    entry_append(db, sid, &sys_msg);

    /* Insert inbox item */
    int64_t iid = inbox_insert(db, sid, "test", "hello");
    assert(iid > 0);

    /* Verify session starts idle */
    const char *check_sql = "SELECT state FROM sessions WHERE id=?;";
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *state = (const char *)sqlite3_column_text(stmt, 0);
    assert(strcmp(state, "idle") == 0);
    sqlite3_finalize(stmt);

    /* Start daemon in background thread */
    shutdown_reset();
    Config cfg = {0};
    cfg.db_path = (char *)DB_PATH;
    cfg.workspace = "/tmp";
    cfg.shell_timeout = 5;
    cfg.provider.base_url = "http://localhost:1/v1"; /* will fail — that's OK */
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 100;
    cfg.provider.context_window = 4000;
    cfg.max_iterations = 1;

    void *args[2] = {&cfg, db};
    pthread_t dt;
    pthread_create(&dt, NULL, daemon_thread, args);

    /* Give daemon time to start epoll loop */
    usleep(100000);

    /* Signal the session */
    daemon_signal_session(sid);

    /* Wait for agent to be forked and reaped (agent will fail since no real LLM) */
    usleep(2000000); /* 2s — agent fork + LLM timeout + reap */

    /* Session should be back to idle after reap */
    sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    state = (const char *)sqlite3_column_text(stmt, 0);
    assert(strcmp(state, "idle") == 0);
    sqlite3_finalize(stmt);

    /* Inbox should be consumed */
    int pending = inbox_count(db, sid);
    assert(pending == 0);

    /* last_route should be set to "test" (the source of our inbox item) */
    char *route = session_get_last_route(db, sid);
    assert(route && strcmp(route, "test") == 0);
    free(route);

    /* Shutdown daemon */
    shutdown_request();
    pthread_join(dt, NULL);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_daemon_fork_reap\n");
}

/* ── T94: Daemon startup recovery ────────────────────────────────── */

static void test_startup_recovery_running(void) {
    /* "running" sessions reset to "idle" */
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "recovery_run", NULL);
    assert(sid > 0);

    /* Force state to "running" (simulates daemon crash mid-agent) */
    sqlite3_exec(db,
        "UPDATE sessions SET state='running', lock_holder='old-daemon' WHERE id=1;",
        NULL, NULL, NULL);

    daemon_startup_recovery(db);

    /* Should be idle now */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT state, lock_holder FROM sessions WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "idle") == 0);
    assert(sqlite3_column_type(stmt, 1) == SQLITE_NULL);
    sqlite3_finalize(stmt);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_startup_recovery_running\n");
}

static void test_startup_recovery_waiting_with_inbox(void) {
    /* "waiting" session with inbox item → "idle" (no error injected) */
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "recovery_wait_inbox", NULL);
    assert(sid > 0);

    /* Force state to "waiting" */
    sqlite3_exec(db,
        "UPDATE sessions SET state='waiting' WHERE id=1;",
        NULL, NULL, NULL);

    /* Sub-agent result already in inbox */
    inbox_insert(db, sid, "sub-agent", "result from child");

    daemon_startup_recovery(db);

    /* Should be idle, inbox still has the item (daemon loop will fork) */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "idle") == 0);
    sqlite3_finalize(stmt);

    /* Inbox count should be 1 (original item, no error added) */
    assert(inbox_count(db, sid) == 1);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_startup_recovery_waiting_with_inbox\n");
}

static void test_startup_recovery_waiting_no_inbox(void) {
    /* "waiting" session with empty inbox → error injected + "idle" */
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid = session_create(db, "recovery_wait_empty", NULL);
    assert(sid > 0);

    /* Force state to "waiting" */
    sqlite3_exec(db,
        "UPDATE sessions SET state='waiting' WHERE id=1;",
        NULL, NULL, NULL);

    /* No inbox items — sub-agent was lost */
    assert(inbox_count(db, sid) == 0);

    daemon_startup_recovery(db);

    /* Should be idle */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?;", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "idle") == 0);
    sqlite3_finalize(stmt);

    /* Error message should have been injected into inbox */
    assert(inbox_count(db, sid) == 1);

    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 1, &count);
    assert(count == 1);
    assert(strstr(items[0].payload, "lost during daemon restart") != NULL);
    assert(strcmp(items[0].source, "recovery") == 0);
    inbox_items_free(items, count);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS test_startup_recovery_waiting_no_inbox\n");
}

int main(void) {
    printf("test_daemon:\n");
    test_signal_pipe_init_and_write();
    test_signal_pipe_multiple();
    test_child_tracking();
    test_last_route();
    test_daemon_fork_reap();
    test_startup_recovery_running();
    test_startup_recovery_waiting_with_inbox();
    test_startup_recovery_waiting_no_inbox();
    printf("All daemon tests passed.\n");
    return 0;
}
