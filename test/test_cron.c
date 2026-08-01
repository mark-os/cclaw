#define _GNU_SOURCE
#include "cron.h"
#include "db.h"
#include "wake.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* cron_jobs.agent_name / sessions.agent_name are enforced FKs (v31): seed
 * every agent name this file schedules for before any child rows land. */
static sqlite3 *open_seeded(const char *path) {
    sqlite3 *db = test_db_open(path);
    if (!db) return NULL;
    static const char *agents[] = {
        "test_agent", "agent_a", "agent_b", "A", "Res", "Ghost", "Hb", NULL
    };
    for (int i = 0; agents[i]; i++)
        test_seed_agent(db, agents[i]);
    return db;
}

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
    localtime_r(&t, &tm);
    assert(tm.tm_hour == 2);
    assert(tm.tm_min == 30);
    printf("  PASS: specific time\n");
}

/* Fields evaluate in the daemon's LOCAL timezone, not UTC. Pinned with a
 * POSIX TZ string rather than "America/New_York" so the test needs no tzdata
 * on the box and can't drift when tzdata ships new rules. */
#define TZ_EASTERN "EST5EDT,M3.2.0/2,M11.1.0/2"

static void set_tz(const char *tz) {
    if (tz) setenv("TZ", tz, 1);
    else unsetenv("TZ");
    tzset();
}

static void test_cron_next_run_local_tz(void) {
    set_tz(TZ_EASTERN);

    /* 2024-01-15 12:00:00 UTC = 07:00 EST. "0 9 * * *" is 9am *local*, i.e.
     * 14:00 UTC the same day — under the old gmtime_r it was 09:00 UTC,
     * already past, and would have landed on the 16th. */
    int64_t base = 1705320000;
    int64_t next = cron_next_run("0 9 * * *", base);
    assert(next == 1705327200);   /* 2024-01-15 14:00:00 UTC */

    struct tm tm;
    time_t t = (time_t)next;
    localtime_r(&t, &tm);
    assert(tm.tm_hour == 9 && tm.tm_min == 0);
    assert(tm.tm_mday == 15);

    set_tz(NULL);
    printf("  PASS: evaluates in local timezone\n");
}

static void test_cron_next_run_dst(void) {
    set_tz(TZ_EASTERN);

    /* Spring forward: 2024-03-10 02:00 EST → 03:00 EDT. A 9am job on the 9th
     * is 14:00 UTC; the next one is 13:00 UTC (23-hour day) — same wall
     * clock, one hour less of elapsed time. */
    int64_t mar9 = 1709992800;    /* 2024-03-09 14:00:00 UTC = 09:00 EST */
    int64_t next = cron_next_run("0 9 * * *", mar9);
    assert(next == 1710075600);   /* 2024-03-10 13:00:00 UTC = 09:00 EDT */
    assert(next - mar9 == 23 * 3600);

    /* Fall back: 2024-11-03 02:00 EDT → 01:00 EST, a 25-hour day. */
    int64_t nov2 = 1730556000;    /* 2024-11-02 14:00:00 UTC = 10:00 EDT */
    next = cron_next_run("0 10 * * *", nov2);
    assert(next == 1730646000);   /* 2024-11-03 15:00:00 UTC = 10:00 EST */
    assert(next - nov2 == 25 * 3600);

    /* An hour that the spring-forward day skips entirely never fires on it:
     * 02:30 on 2024-03-10 does not exist, so the next 02:30 is the 11th. */
    int64_t mar10 = 1710046800;   /* 2024-03-10 05:00:00 UTC = 00:00 EST */
    next = cron_next_run("30 2 * * *", mar10);
    struct tm tm;
    time_t t = (time_t)next;
    localtime_r(&t, &tm);
    assert(tm.tm_mday == 11 && tm.tm_hour == 2 && tm.tm_min == 30);

    set_tz(NULL);
    printf("  PASS: follows DST transitions\n");
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
    localtime_r(&t, &tm);
    assert(tm.tm_min % 15 == 0);
    printf("  PASS: step expression\n");
}

static void test_cron_crud(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);

    int64_t sid = session_create(db, "cron_test", NULL, -1, 0);
    assert(sid > 0);

    int64_t jid = cron_add(db, "test_agent", "test_job", "0 * * * *", 0, 0, sid, "hello");
    assert(jid > 0);

    int count = 0;
    CronJob *jobs = cron_list(db, "test_agent", &count);
    assert(count == 1);
    assert(strcmp(jobs[0].name, "test_job") == 0);
    assert(strcmp(jobs[0].cron_expr, "0 * * * *") == 0);
    assert(strcmp(jobs[0].task, "hello") == 0);
    assert(strcmp(jobs[0].kind, "task") == 0);
    assert(jobs[0].enabled == 1);
    assert(jobs[0].next_run_at > 0);
    cron_list_free(jobs, count);

    /* Agent scoping (review-2 F2): another agent cannot delete this job */
    assert(cron_remove(db, jid, "other_agent") == -1);
    jobs = cron_list(db, "test_agent", &count);
    assert(count == 1);
    cron_list_free(jobs, count);

    assert(cron_remove(db, jid, "test_agent") == 0);
    jobs = cron_list(db, "test_agent", &count);
    assert(count == 0 && jobs == NULL);

    assert(cron_remove(db, 999, "test_agent") == -1);
    assert(cron_add(db, "test_agent", "bad", "invalid", 0, 0, sid, "x") == -1);

    db_close(db);
    printf("  PASS: CRUD operations\n");
}

static void test_cron_table_created(void) {
    sqlite3 *db = open_seeded(":memory:");
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

static void test_cron_agent_isolation(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);

    int64_t sid = session_create(db, "test", NULL, -1, 0);
    cron_add(db, "agent_a", "job_a", "0 * * * *", 0, 0, sid, "task_a");
    cron_add(db, "agent_b", "job_b", "0 * * * *", 0, 0, sid, "task_b");

    int count = 0;
    CronJob *jobs = cron_list(db, "agent_a", &count);
    assert(count == 1);
    assert(strcmp(jobs[0].name, "job_a") == 0);
    cron_list_free(jobs, count);

    jobs = cron_list(db, "agent_b", &count);
    assert(count == 1);
    assert(strcmp(jobs[0].name, "job_b") == 0);
    cron_list_free(jobs, count);

    jobs = cron_list(db, "nonexistent", &count);
    assert(count == 0 && jobs == NULL);

    db_close(db);
    printf("  PASS: agent isolation\n");
}

/* ── Schedule validation (cron_add) ───────────────────────────────────── */

static void test_cron_add_schedule_validation(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);
    int64_t sid = session_create(db, "v", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    /* Exactly one schedule type required. */
    assert(cron_add(db, "A", "none", "", 0, 0, sid, "t") == -1);          /* zero specs */
    assert(cron_add(db, "A", "two", "0 * * * *", now + 3600, 0, sid, "t") == -1); /* expr+run_at */

    /* One-shot: must be in the future by at least the floor (300). */
    assert(cron_add(db, "A", "past", "", now - 10, 0, sid, "t") == -1);
    assert(cron_add(db, "A", "soon", "", now + 60, 0, sid, "t") == -1);   /* under floor */
    assert(cron_add(db, "A", "ok",   "", now + 3600, 0, sid, "t") > 0);

    /* Interval: must be >= floor. */
    assert(cron_add(db, "A", "izero", "", 0, 0, sid, "t") == -1);          /* interval 0 = unused */
    assert(cron_add(db, "A", "ismall", "", 0, 60, sid, "t") == -1);        /* under floor */
    assert(cron_add(db, "A", "iok",   "", 0, 600, sid, "t") > 0);

    db_close(db);
    printf("  PASS: schedule validation\n");
}

static void test_cron_add_floor_fire_to_fire(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);
    int64_t sid = session_create(db, "f", "A", -1, 0);

    /* Hourly always passes regardless of when created — the floor is measured
     * fire-to-fire (3600s), not off the possibly-tiny first-fire delta. */
    assert(cron_add(db, "A", "hourly", "0 * * * *", 0, 0, sid, "t") > 0);
    /* Every-minute fails: fire-to-fire 60s < floor 300s. */
    assert(cron_add(db, "A", "minutely", "* * * * *", 0, 0, sid, "t") == -1);

    db_close(db);
    printf("  PASS: fire-to-fire floor\n");
}

/* ── Dispatch (run_due_jobs via cron_run_due) ─────────────────────────── */

/* Insert a due job with full control over schedule fields. */
static int64_t insert_due(sqlite3 *db, const char *agent, const char *kind,
                          const char *expr, int64_t run_at, int64_t interval_s,
                          int64_t session_id, const char *task, int64_t next_run_at) {
    /* Names are unique per (agent, name) since v41 — number them so a test
     * can still stage two rival rows for the same agent. */
    static int seq = 0;
    char jname[16];
    snprintf(jname, sizeof(jname), "j%d", ++seq);
    const char *sql =
        "INSERT INTO cron_jobs(agent_name,name,kind,cron_expr,run_at,interval_s,"
        " session_id,task,enabled,next_run_at) VALUES(?,?,?,?,?,?,?,?,1,?);";
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, jname, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 4, expr, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 5, run_at);
    sqlite3_bind_int64(st, 6, interval_s);
    sqlite3_bind_int64(st, 7, session_id);
    sqlite3_bind_text(st, 8, task, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 9, next_run_at);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db);
}

static int64_t job_count(sqlite3 *db, int64_t id) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM cron_jobs WHERE id=?",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    int64_t n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static int64_t job_next(sqlite3 *db, int64_t id) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT next_run_at FROM cron_jobs WHERE id=?",
                              -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    int64_t n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Count inbox rows matching source (NULL = any) with payload substring. */
static int inbox_match(sqlite3 *db, int64_t sid, const char *source,
                       const char *needle) {
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 20, &count);
    int hits = 0;
    for (int i = 0; i < count; i++) {
        if (source && strcmp(items[i].source, source) != 0) continue;
        if (needle && !strstr(items[i].payload, needle)) continue;
        hits++;
    }
    inbox_items_free(items, count);
    return hits;
}

static void test_dispatch_oneshot(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    int64_t jid = insert_due(db, "A", "task", "", now, 0, sid, "ping", now);
    cron_run_due(db);

    assert(inbox_match(db, sid, "cron", "ping") == 1);
    assert(job_count(db, jid) == 0);   /* one-shot self-deletes */

    /* Fire again: nothing to re-fire, still exactly one inbox row. */
    cron_run_due(db);
    assert(inbox_match(db, sid, "cron", "ping") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: one-shot fires once and self-deletes\n");
}

static void test_dispatch_interval(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    int64_t jid = insert_due(db, "A", "task", "", 0, 600, sid, "tick", now - 1);
    cron_run_due(db);

    assert(inbox_match(db, sid, "cron", "tick") == 1);
    assert(job_count(db, jid) == 1);              /* persists */
    assert(job_next(db, jid) >= now + 600 - 2);   /* advanced by interval */

    wake_close();
    db_close(db);
    printf("  PASS: interval reschedules\n");
}

static void test_dispatch_session_zero_resolves(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "Res", -1, 0);
    int64_t now = (int64_t)time(NULL);

    /* session_id=0 task resolves to the agent's most recent session. */
    insert_due(db, "Res", "task", "", 0, 600, 0, "resolved", now - 1);
    cron_run_due(db);
    assert(inbox_match(db, sid, "cron", "resolved") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: session_id=0 resolves to recent session\n");
}

static void test_dispatch_session_zero_no_session(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t now = (int64_t)time(NULL);

    /* Agent with no sessions: WARN-skip, no crash, row still reschedules. */
    int64_t jid = insert_due(db, "Ghost", "task", "", 0, 600, 0, "nowhere", now - 1);
    cron_run_due(db);
    assert(job_count(db, jid) == 1);
    assert(job_next(db, jid) >= now + 600 - 2);

    wake_close();
    db_close(db);
    printf("  PASS: session_id=0 with no session skips cleanly\n");
}

static void test_dispatch_heartbeat_idle(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "Hb", -1, 0);
    /* Touch updated_at so the session is recent (idle by default). */
    Message um = {.role = ROLE_USER, .content = "hi"};
    entry_append_with_iteration(db, sid, &um, 1);
    int64_t now = (int64_t)time(NULL);

    insert_due(db, "Hb", "heartbeat", "", 0, 1800, 0, "", now);
    cron_run_due(db);

    assert(inbox_match(db, sid, "heartbeat", "HEARTBEAT_OK") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: heartbeat fires into idle session\n");
}

static void test_dispatch_heartbeat_skips_busy(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "Hb", -1, 0);
    char sql[128];
    snprintf(sql, sizeof(sql),
             "UPDATE sessions SET state='running' WHERE id=%lld", (long long)sid);
    sqlite3_exec(db, sql, NULL, NULL, NULL);
    int64_t now = (int64_t)time(NULL);

    insert_due(db, "Hb", "heartbeat", "", 0, 1800, 0, "", now);
    cron_run_due(db);

    assert(inbox_match(db, sid, "heartbeat", NULL) == 0);   /* no idle session */

    wake_close();
    db_close(db);
    printf("  PASS: heartbeat skips busy agent\n");
}

static void test_dispatch_heartbeat_no_stack(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "Hb", -1, 0);
    Message um = {.role = ROLE_USER, .content = "hi"};
    entry_append_with_iteration(db, sid, &um, 1);
    int64_t now = (int64_t)time(NULL);

    /* Two due heartbeat rows: only one unconsumed pulse should ever land. */
    insert_due(db, "Hb", "heartbeat", "", 0, 1800, 0, "", now);
    insert_due(db, "Hb", "heartbeat", "", 0, 1800, 0, "", now);
    cron_run_due(db);
    cron_run_due(db);

    assert(inbox_match(db, sid, "heartbeat", NULL) == 1);   /* never stacked */

    wake_close();
    db_close(db);
    printf("  PASS: heartbeat never stacks pulses\n");
}

int main(void) {
    TEST_INIT();
    printf("test_cron:\n");
    test_cron_table_created();
    test_cron_next_run_every_minute();
    test_cron_next_run_specific();
    test_cron_next_run_invalid();
    test_cron_next_run_step();
    test_cron_next_run_local_tz();
    test_cron_next_run_dst();
    test_cron_crud();
    test_cron_agent_isolation();
    test_cron_add_schedule_validation();
    test_cron_add_floor_fire_to_fire();
    test_dispatch_oneshot();
    test_dispatch_interval();
    test_dispatch_session_zero_resolves();
    test_dispatch_session_zero_no_session();
    test_dispatch_heartbeat_idle();
    test_dispatch_heartbeat_skips_busy();
    test_dispatch_heartbeat_no_stack();
    printf("ALL PASSED\n");
    return 0;
}
