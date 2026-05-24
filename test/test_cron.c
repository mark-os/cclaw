#define _GNU_SOURCE
#include "cron.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void test_cron_next_run_every_minute(void) {
    /* every minute */
    int64_t now = 1700000000;
    int64_t next = cron_next_run("* * * * *", now);
    assert(next > now);
    assert(next % 60 == 0); /* must be on a minute boundary */
    assert(next <= now + 120); /* within 2 minutes */
    printf("  PASS: every minute\n");
}

static void test_cron_next_run_specific(void) {
    /* 02:30 every day */
    int64_t base = 1705280400; /* 2024-01-15 01:00:00 UTC */
    int64_t next = cron_next_run("30 2 * * *", base);
    assert(next > base);
    struct tm tm;
    time_t t = (time_t)next;
    gmtime_r(&t, &tm);
    assert(tm.tm_hour == 2);
    assert(tm.tm_min == 30);
    printf("  PASS: specific time\n");
}

static void test_cron_next_run_invalid(void) {
    assert(cron_next_run(NULL, 0) == -1);
    assert(cron_next_run("", 0) == -1);
    assert(cron_next_run("bad", 0) == -1);
    assert(cron_next_run("60 * * * *", 0) == -1);
    printf("  PASS: invalid expressions\n");
}

static void test_cron_next_run_step(void) {
    /* every 15 minutes */
    int64_t base = 1700000000;
    int64_t next = cron_next_run("*/15 * * * *", base);
    assert(next > base);
    struct tm tm;
    time_t t = (time_t)next;
    gmtime_r(&t, &tm);
    assert(tm.tm_min % 15 == 0);
    printf("  PASS: step expression\n");
}

static void test_cron_crud(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    int64_t sid = session_create(db, "cron_test", NULL);
    assert(sid > 0);

    int64_t jid = cron_add(db, "test_job", "0 * * * *", sid, "hello");
    assert(jid > 0);

    int count = 0;
    CronJob *jobs = cron_list(db, sid, &count);
    assert(count == 1);
    assert(strcmp(jobs[0].name, "test_job") == 0);
    assert(strcmp(jobs[0].cron_expr, "0 * * * *") == 0);
    assert(strcmp(jobs[0].task, "hello") == 0);
    assert(jobs[0].enabled == 1);
    assert(jobs[0].next_run_at > 0);
    cron_list_free(jobs, count);

    assert(cron_remove(db, jid) == 0);
    jobs = cron_list(db, sid, &count);
    assert(count == 0 && jobs == NULL);

    assert(cron_remove(db, 999) == -1);
    assert(cron_add(db, "bad", "invalid", sid, "x") == -1);

    db_close(db);
    printf("  PASS: CRUD operations\n");
}

static void test_cron_table_created(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name='cron_jobs';",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("  PASS: table exists\n");
}

int main(void) {
    printf("test_cron:\n");
    test_cron_table_created();
    test_cron_next_run_every_minute();
    test_cron_next_run_specific();
    test_cron_next_run_invalid();
    test_cron_next_run_step();
    test_cron_crud();
    printf("ALL PASSED\n");
    return 0;
}
