#define _GNU_SOURCE
#include "config_registry.h"
#include "cron.h"
#include "db.h"
#include "wake.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* cron_jobs.agent_name / sessions.agent_name are enforced FKs (v31): seed
 * every agent name this file schedules for before any child rows land. */
static sqlite3 *open_seeded(const char *path) {
    sqlite3 *db = test_db_open(path);
    if (!db) return NULL;
    static const char *agents[] = {
        "test_agent", "agent_a", "agent_b", "A", "Res", "Ghost", "Hb",
        "Chat", "Other", "New", "Fail", NULL
    };
    for (int i = 0; agents[i]; i++)
        test_seed_agent(db, agents[i]);
    return db;
}

static void exec_ok(sqlite3 *db, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));
static void exec_ok(sqlite3 *db, const char *fmt, ...) {
    char sql[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sql, sizeof(sql), fmt, ap);
    va_end(ap);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

static int64_t scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    int64_t v = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
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

/* Recurring job through the storage-layer entry point the tool uses. */
static int64_t add_recurring(sqlite3 *db, const char *agent, const char *name,
                             const char *expr, int64_t sid, const char *prompt) {
    char doc[512];
    snprintf(doc, sizeof(doc),
             "{\"name\":\"%s\",\"cron_expr\":\"%s\",\"prompt\":\"%s\"}",
             name, expr, prompt);
    return cron_upsert(db, agent, sid, doc, NULL, NULL);
}

static void test_cron_crud(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);

    int64_t sid = session_create(db, "cron_test", NULL, -1, 0);
    assert(sid > 0);

    int64_t jid = add_recurring(db, "test_agent", "test_job", "0 * * * *", sid, "hello");
    assert(jid > 0);

    int count = 0;
    CronJob *jobs = cron_list(db, "test_agent", &count);
    assert(count == 1);
    assert(strcmp(jobs[0].name, "test_job") == 0);
    assert(strcmp(jobs[0].cron_expr, "0 * * * *") == 0);
    assert(strcmp(jobs[0].task, "hello") == 0);
    assert(jobs[0].script == NULL);   /* prompt-only payload */
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
    assert(add_recurring(db, "test_agent", "bad", "invalid", sid, "x") == -1);

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
    add_recurring(db, "agent_a", "job_a", "0 * * * *", sid, "task_a");
    add_recurring(db, "agent_b", "job_b", "0 * * * *", sid, "task_b");

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

/* ── Schedule validation (cron_upsert) ────────────────────────────────── */

/* One-shot job by delay. */
static int64_t add_oneshot(sqlite3 *db, const char *agent, const char *name,
                           int64_t in_seconds, int64_t sid) {
    char doc[256];
    snprintf(doc, sizeof(doc),
             "{\"name\":\"%s\",\"in_seconds\":%lld,\"prompt\":\"t\"}",
             name, (long long)in_seconds);
    return cron_upsert(db, agent, sid, doc, NULL, NULL);
}

static void test_cron_schedule_validation(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);
    int64_t sid = session_create(db, "v", "A", -1, 0);

    /* A new job must carry a schedule; two schedules is ambiguous. */
    char *err = NULL;
    assert(cron_upsert(db, "A", sid, "{\"name\":\"none\",\"prompt\":\"t\"}",
                       NULL, &err) == -1);
    assert(err && strstr(err, "no schedule"));
    free(err);
    assert(cron_upsert(db, "A", sid,
        "{\"name\":\"two\",\"cron_expr\":\"0 * * * *\",\"in_seconds\":3600,"
        "\"prompt\":\"t\"}", NULL, NULL) == -1);

    /* One-shot: the delay must clear the floor (default 30s). */
    assert(add_oneshot(db, "A", "past", -10, sid) == -1);
    assert(add_oneshot(db, "A", "soon", 20, sid) == -1);
    assert(add_oneshot(db, "A", "ok", 3600, sid) > 0);
    assert(add_oneshot(db, "A", "floor", 30, sid) > 0);   /* exactly the floor */

    db_close(db);
    printf("  PASS: schedule validation\n");
}

static void test_cron_floor_fire_to_fire(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);
    int64_t sid = session_create(db, "f", "A", -1, 0);

    /* Hourly always passes regardless of when created — the floor is measured
     * fire-to-fire (3600s), not off the possibly-tiny first-fire delta. */
    assert(add_recurring(db, "A", "hourly", "0 * * * *", sid, "t") > 0);
    /* Every-minute clears the 30s default floor; raising the floor above the
     * fire-to-fire spacing is what refuses it. */
    assert(add_recurring(db, "A", "minutely", "* * * * *", sid, "t") > 0);
    assert(config_set(db, "cron_min_interval_seconds", "300") == 0);
    assert(add_recurring(db, "A", "minutely2", "* * * * *", sid, "t") == -1);
    assert(add_recurring(db, "A", "hourly2", "0 * * * *", sid, "t") > 0);

    db_close(db);
    printf("  PASS: fire-to-fire floor\n");
}

/* The verdicts the tool turns into two different messages. */
static void test_cron_schedule_check_codes(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);
    int64_t next = 0, every = 0;

    assert(cron_schedule_check(db, NULL, 0, &next, &every) == CRON_SCHED_NONE);
    assert(cron_schedule_check(db, "0 * * * *", 60, &next, &every) == CRON_SCHED_BOTH);
    assert(cron_schedule_check(db, "nonsense", 0, &next, &every) == CRON_SCHED_BAD_EXPR);
    assert(cron_schedule_check(db, NULL, 5, &next, &every) == CRON_SCHED_FLOOR);
    assert(every == 5);   /* echoed back for the refusal message */
    assert(cron_schedule_check(db, "0 * * * *", 0, &next, &every) == CRON_SCHED_OK);
    assert(every == 3600 && next > (int64_t)time(NULL));

    db_close(db);
    printf("  PASS: schedule check codes\n");
}

/* ── Dispatch (run_due_jobs via cron_run_due) ─────────────────────────── */

/* Insert a due job with full control over schedule fields. */
static int64_t insert_due(sqlite3 *db, const char *agent,
                          const char *expr, int64_t run_at, int64_t interval_s,
                          int64_t session_id, const char *task, int64_t next_run_at) {
    /* Names are unique per (agent, name) since v41 — number them so a test
     * can still stage two rival rows for the same agent. */
    static int seq = 0;
    char jname[16];
    snprintf(jname, sizeof(jname), "j%d", ++seq);
    const char *sql =
        "INSERT INTO cron_jobs(agent_name,name,cron_expr,run_at,interval_s,"
        " session_id,task,enabled,next_run_at) VALUES(?,?,?,?,?,?,?,1,?);";
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, jname, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, expr, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, run_at);
    sqlite3_bind_int64(st, 5, interval_s);
    sqlite3_bind_int64(st, 6, session_id);
    sqlite3_bind_text(st, 7, task, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 8, next_run_at);
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

    int64_t jid = insert_due(db, "A", "", now, 0, sid, "ping", now);
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

    int64_t jid = insert_due(db, "A", "", 0, 600, sid, "tick", now - 1);
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
    insert_due(db, "Res", "", 0, 600, 0, "resolved", now - 1);
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
    int64_t jid = insert_due(db, "Ghost", "", 0, 600, 0, "nowhere", now - 1);
    cron_run_due(db);
    assert(job_count(db, jid) == 1);
    assert(job_next(db, jid) >= now + 600 - 2);

    wake_close();
    db_close(db);
    printf("  PASS: session_id=0 with no session skips cleanly\n");
}

/* ── The agent pulse: an ordinary bare-wake job ───────────────────────
 * There is no kind='heartbeat' any more. The three tests below are the old
 * pulse tests re-aimed at the generalized machinery that replaced it: a
 * payload-less job that late-resolves its session, nudges it, and cannot
 * stack anything. */

/* Drain the wake pipe (non-blocking, so an empty pipe returns immediately).
 * Returns how many nudges were queued; *last gets the final session id. */
static int wake_drain(int64_t *last) {
    WakeMsg m;
    int n = 0;
    while (read(wake_fd(), &m, sizeof(m)) == (ssize_t)sizeof(m)) {
        if (last) *last = m.session_id;
        n++;
    }
    return n;
}

static void test_seed_heartbeat_shape(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);
    assert(cron_seed_heartbeat(db, "Hb") == 0);
    assert(cron_seed_heartbeat(db, "Hb") == 0);   /* idempotent by name */

    int count = 0;
    CronJob *jobs = cron_list(db, "Hb", &count);
    assert(count == 1);
    assert(strcmp(jobs[0].name, "heartbeat") == 0);
    assert(strcmp(jobs[0].cron_expr, "*/30 * * * *") == 0);
    assert(jobs[0].task && jobs[0].task[0] == '\0');  /* no prompt  → bare wake */
    assert(jobs[0].script == NULL);                   /* no script  → bare wake */
    assert(jobs[0].run_at == 0 && jobs[0].interval_s == 0);
    assert(jobs[0].session_id == 0);                  /* resolved at fire time */
    assert(jobs[0].enabled == 0);                     /* opt-in, costs a turn */
    cron_list_free(jobs, count);

    db_close(db);
    printf("  PASS: seeded heartbeat is a disabled bare-wake job\n");
}

/* session_id=0 late-resolves to the agent's most recent session, and a bare
 * wake adds nothing of its own — it just gets that session moving on what is
 * already queued there. */
static void test_fire_bare_wake_nudges_session(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "Hb", -1, 0);
    inbox_insert(db, sid, "channel", NULL, "queued while asleep");
    int64_t now = (int64_t)time(NULL);
    wake_drain(NULL);

    insert_due(db, "Hb", "*/30 * * * *", 0, 0, 0, "", now - 1);
    cron_run_due(db);

    int64_t woke = 0;
    assert(wake_drain(&woke) == 1 && woke == sid);
    assert(inbox_match(db, sid, NULL, NULL) == 1);   /* only the queued row */

    wake_close();
    db_close(db);
    printf("  PASS: bare wake resolves session_id=0 and nudges that session\n");
}

/* The old pulse branch refused a busy agent outright (it was about to insert a
 * prompt). A bare wake needs no such rule: it writes nothing, so the nudge is
 * absorbed by the turn boundary — the mid-turn invariant is never at risk. */
static void test_fire_bare_wake_busy_session(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "Hb", -1, 0);
    exec_ok(db, "UPDATE sessions SET state='llm_running' WHERE id=%lld",
            (long long)sid);
    int64_t now = (int64_t)time(NULL);
    wake_drain(NULL);

    insert_due(db, "Hb", "*/30 * * * *", 0, 0, 0, "", now - 1);
    cron_run_due(db);

    assert(inbox_match(db, sid, NULL, NULL) == 0);   /* nothing written */
    assert(wake_drain(NULL) == 1);                   /* just a nudge */

    wake_close();
    db_close(db);
    printf("  PASS: bare wake into a busy session writes nothing\n");
}

/* Two rival pulses and a prompt job, fired twice: the bare wakes have no
 * payload to stack, and the prompt job's fire coalesces on its own undrained
 * row — together, the generalized form of "never stack a pulse". */
static void test_fire_bare_wake_no_stack(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "Hb", -1, 0);
    int64_t now = (int64_t)time(NULL);

    insert_due(db, "Hb", "", 0, 600, sid, "", now - 1);
    insert_due(db, "Hb", "", 0, 600, sid, "", now - 1);
    insert_due(db, "Hb", "", 0, 600, sid, "pulse", now - 1);
    cron_run_due(db);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);

    assert(inbox_match(db, sid, NULL, NULL) == 1);
    assert(inbox_match(db, sid, "cron", "pulse") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: bare wakes never stack\n");
}

/* ── Fire path: target resolution, payload dispatch, guards ───────────── */

/* Full control over every fire-relevant column, so a test can stage the exact
 * job shape the fire path has to resolve. */
typedef struct {
    const char *agent, *name, *expr, *task, *script;
    const char *target, *target_agent, *channel, *chat;
    int64_t run_at, interval_s, session_id, next_run_at;
} JobSpec;

static int64_t insert_job(sqlite3 *db, const JobSpec *j) {
    const char *sql =
        "INSERT INTO cron_jobs(agent_name,name,cron_expr,run_at,interval_s,"
        " session_id,task,script,target,target_agent,channel_name,chat_id,"
        " enabled,next_run_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,1,?);";
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, j->agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, j->name, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, j->expr ? j->expr : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 4, j->run_at);
    sqlite3_bind_int64(st, 5, j->interval_s);
    sqlite3_bind_int64(st, 6, j->session_id);
    sqlite3_bind_text(st, 7, j->task ? j->task : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 8, j->script, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 9, j->target, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 10, j->target_agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 11, j->channel, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 12, j->chat, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 13, j->next_run_at);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db);
}

static void route_pin(sqlite3 *db, const char *chan, const char *chat, int64_t sid) {
    exec_ok(db, "INSERT INTO channel_routes(channel_name,chat_id,session_id)"
                " VALUES('%s','%s',%lld)", chan, chat, (long long)sid);
}

/* source_ref carries the job NAME, not its id: ids die with one-shot removal
 * and delete-recreate cycles, names are the logical identity. */
static void test_fire_stamps_source_ref(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "A", .name = "nightly", .interval_s = 600,
                 .session_id = sid, .task = "report", .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 1);
    assert(strcmp(items[0].source, "cron") == 0);
    assert(items[0].source_ref && strcmp(items[0].source_ref, "nightly") == 0);
    inbox_items_free(items, count);

    wake_close();
    db_close(db);
    printf("  PASS: fire stamps the job name as source_ref\n");
}

/* Follow-the-conversation: the chat is the durable identity, so the fire goes
 * to whatever session is routed there NOW — not the one stamped at set time. */
static void test_fire_follows_route(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t stamped = session_create(db, "old", "Chat", -1, 0);
    int64_t current = session_create(db, "new", "Chat", -1, 0);
    route_pin(db, "tg", "42", current);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "Chat", .name = "follow", .interval_s = 600,
                 .session_id = stamped, .task = "ping", .channel = "tg",
                 .chat = "42", .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    assert(inbox_match(db, current, "cron", "ping") == 1);
    assert(inbox_match(db, stamped, "cron", "ping") == 0);

    wake_close();
    db_close(db);
    printf("  PASS: follow-the-chat resolves the routed session\n");
}

/* No route for that chat any more (it was never re-pinned, or the row was
 * dropped): the stamped session is the fallback, not an error. */
static void test_fire_route_gone_falls_back(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t stamped = session_create(db, "old", "Chat", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "Chat", .name = "follow", .interval_s = 600,
                 .session_id = stamped, .task = "ping", .channel = "tg",
                 .chat = "42", .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    assert(inbox_match(db, stamped, "cron", "ping") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: route gone falls back to the stamped session\n");
}

/* Authority is re-checked at fire time: a chat re-routed to another agent is
 * no longer reachable through a standing job. */
static void test_fire_authority_lost(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t owner = session_create(db, "own", "Chat", -1, 0);
    int64_t theirs = session_create(db, "theirs", "Other", -1, 0);
    route_pin(db, "tg", "42", theirs);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "Chat", .name = "steal", .interval_s = 600,
                 .session_id = owner, .task = "ping", .channel = "tg",
                 .chat = "42", .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    assert(inbox_match(db, theirs, NULL, NULL) == 0);      /* never fires */
    assert(inbox_match(db, owner, "cron_error", "no longer routes") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: fire-time authority loss errors to the owner\n");
}

/* A pin is stale-by-choice — but a vanished session is reported, not silent. */
static void test_fire_pin_gone_errors(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t owner = session_create(db, "own", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "A", .name = "pinned", .interval_s = 600,
                 .session_id = 99999, .task = "ping", .target = "pin",
                 .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    assert(inbox_match(db, owner, "cron_error", "pinned session") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: pinned session gone errors to the owner\n");
}

/* session:"new" with channel fields: a fresh session per fire, stamped with
 * the chat so ordinary delivery reaches it. */
static void test_fire_new_session_chat(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t bound = session_create(db, "bound", "Chat", -1, 0);
    route_pin(db, "tg", "42", bound);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "Chat", .name = "digest", .interval_s = 600,
                 .task = "summarize", .target = "new", .channel = "tg",
                 .chat = "42", .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);

    int64_t fresh = scalar(db, "SELECT id FROM sessions WHERE name='cron'");
    assert(fresh > 0 && fresh != bound);
    assert(inbox_match(db, fresh, "cron", "summarize") == 1);
    assert(inbox_match(db, bound, "cron", "summarize") == 0);
    assert(scalar(db, "SELECT chat_id='42' AND channel_name='tg' FROM sessions"
                      " WHERE name='cron'") == 1);
    /* the fired session is recorded — skip-if-busy reads it next time */
    char q[128];
    snprintf(q, sizeof(q), "SELECT session_id FROM cron_jobs WHERE id=%lld",
             (long long)jid);
    assert(scalar(db, q) == fresh);

    wake_close();
    db_close(db);
    printf("  PASS: session:\"new\" with a chat fires into a fresh bound session\n");
}

/* session:"new" without channel fields: parented to the late-resolved owner,
 * so the result rides the existing notify_parent push. */
static void test_fire_new_session_parented(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t owner = session_create(db, "own", "New", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "New", .name = "checkin", .interval_s = 600,
                 .task = "status?", .target = "new", .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    int64_t fresh = scalar(db, "SELECT id FROM sessions WHERE name='cron'");
    assert(fresh > 0);
    assert(inbox_match(db, fresh, "cron", "status?") == 1);
    char q[160];
    snprintf(q, sizeof(q),
             "SELECT parent_session_id=%lld AND parent_tool_call_id IS NULL"
             " FROM sessions WHERE id=%lld", (long long)owner, (long long)fresh);
    assert(scalar(db, q) == 1);

    wake_close();
    db_close(db);
    printf("  PASS: session:\"new\" without a chat parents to the owner\n");
}

/* Recurring 'new' fires must not chain: each fresh session parents to the
 * owner, not to the previous fire's session — chaining walks depth up to
 * agent_max_depth and locks launch_agent out by the second fire. */
static void test_fire_new_session_no_depth_chain(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t owner = session_create(db, "own", "New", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "New", .name = "daily", .interval_s = 600,
                 .task = "report", .target = "new", .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);
    int64_t first = scalar(db, "SELECT id FROM sessions WHERE name='cron'");
    assert(first > 0);
    /* First fire done and idle; owner deliberately stays older than it. */
    exec_ok(db, "UPDATE sessions SET updated_at=updated_at+10 WHERE id=%lld",
            (long long)first);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld WHERE id=%lld",
            (long long)(now - 1), (long long)jid);
    cron_run_due(db);
    char q[192];
    snprintf(q, sizeof(q),
             "SELECT parent_session_id=%lld AND depth=1 FROM sessions"
             " WHERE name='cron' AND id<>%lld", (long long)owner, (long long)first);
    assert(scalar(db, q) == 1);

    wake_close();
    db_close(db);
    printf("  PASS: 'new' fires parent to the owner, never the previous fire\n");
}

/* Setting a 'new' job must not seed skip-if-busy's interlock with the
 * caller's chat session — that would skip the first fires whenever the human
 * who set the job is mid-conversation. */
static void test_upsert_new_seeds_no_interlock(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t caller = session_create(db, "chat", "New", -1, 0);
    assert(cron_upsert(db, "New",  caller,
        "{\"name\":\"digest\",\"cron_expr\":\"0 * * * *\",\"prompt\":\"go\","
        "\"session\":\"new\"}", NULL, NULL) > 0);
    assert(scalar(db, "SELECT session_id FROM cron_jobs WHERE name='digest'") == 0);

    /* But re-setting an already-'new' job keeps the previous fire's stamp. */
    exec_ok(db, "UPDATE cron_jobs SET session_id=%lld WHERE name='digest'",
            (long long)caller);
    assert(cron_upsert(db, "New", caller,
        "{\"name\":\"digest\",\"prompt\":\"go faster\"}", NULL, NULL) > 0);
    char q[128];
    snprintf(q, sizeof(q), "SELECT session_id=%lld FROM cron_jobs"
             " WHERE name='digest'", (long long)caller);
    assert(scalar(db, q) == 1);

    wake_close();
    db_close(db);
    printf("  PASS: a new-mode upsert seeds no caller interlock\n");
}

/* One outstanding fire per job: the second tick coalesces onto the first, and
 * only a drain unblocks the next one. */
static void test_fire_coalesces(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "A", .name = "tick", .interval_s = 600,
                 .session_id = sid, .task = "poll", .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld WHERE id=%lld",
            (long long)(now - 1), (long long)jid);
    cron_run_due(db);
    assert(inbox_match(db, sid, "cron", "poll") == 1);   /* not stacked */

    assert(inbox_consume_into_entries(db, sid, 10) == 1);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld WHERE id=%lld",
            (long long)(now - 1), (long long)jid);
    cron_run_due(db);
    assert(inbox_match(db, sid, "cron", "poll") == 1);   /* fires again once drained */

    wake_close();
    db_close(db);
    printf("  PASS: fires coalesce per job until drained\n");
}

/* Skip-if-busy covers what coalescing cannot: a 'new' fire targets a session
 * that did not exist yet, so the previous fire's session is the interlock. */
static void test_fire_skip_if_busy(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t owner = session_create(db, "own", "New", -1, 0);
    (void)owner;
    int64_t busy = session_create(db, "prev", "New", -1, 0);
    exec_ok(db, "UPDATE sessions SET state='llm_running' WHERE id=%lld",
            (long long)busy);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "New", .name = "digest", .interval_s = 600,
                 .session_id = busy, .task = "again", .target = "new",
                 .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);
    assert(scalar(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 0);

    /* Once it goes idle the next fire proceeds. */
    exec_ok(db, "UPDATE sessions SET state='idle' WHERE id=%lld", (long long)busy);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);
    assert(scalar(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: skip-if-busy holds a fresh-session job to one live fire\n");
}

/* Neither prompt nor script: advance the session, annotate nothing. */
static void test_fire_bare_wake(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "A", .name = "pulse", .interval_s = 600,
                 .session_id = sid, .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);
    assert(inbox_match(db, sid, NULL, NULL) == 0);

    /* 'new' + bare wake is legal but empty — it must not leave a session behind. */
    JobSpec n = {.agent = "New", .name = "empty", .interval_s = 600,
                 .target = "new", .next_run_at = now - 1};
    insert_job(db, &n);
    cron_run_due(db);
    assert(scalar(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 0);

    wake_close();
    db_close(db);
    printf("  PASS: bare wake inserts nothing\n");
}

/* Stage one finished fire: the drained user entry that opened the turn, plus
 * the assistant entry that ended it. stop_reason 4 = STOP_REASON_ERROR. */
static void stage_fire(sqlite3 *db, int64_t sid, const char *job, int failed) {
    static int64_t turn = 1000;
    turn++;
    exec_ok(db, "INSERT INTO entries(session_id,parent_id,turn_id,type,role,content,data)"
                " VALUES(%lld,-1,%lld,'user_message',1,'[cron: %s] go',"
                "        json_object('source','cron','source_ref','%s'))",
            (long long)sid, (long long)turn, job, job);
    exec_ok(db, "INSERT INTO entries(session_id,parent_id,turn_id,type,role,"
                " content,stop_reason)"
                " VALUES(%lld,-1,%lld,'assistant_message',2,'%s',%d)",
            (long long)sid, (long long)turn,
            failed ? "error: the model refused [resp #7]" : "done", failed ? 4 : 1);
}

static void test_fire_failure_auto_pause(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "Fail", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "Fail", .name = "broken", .interval_s = 600,
                 .session_id = sid, .task = "go", .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);
    char q[128];
    snprintf(q, sizeof(q), "SELECT enabled FROM cron_jobs WHERE id=%lld", (long long)jid);

    /* Two failures is not yet a streak. */
    stage_fire(db, sid, "broken", 1);
    stage_fire(db, sid, "broken", 1);
    cron_run_due(db);
    assert(scalar(db, q) == 1);
    assert(inbox_match(db, sid, "cron", "go") == 1);      /* still firing */
    assert(inbox_consume_into_entries(db, sid, 10) == 1);

    /* The third trips it: enabled=0, and the owner is told, with the error. */
    stage_fire(db, sid, "broken", 1);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);
    assert(scalar(db, q) == 0);
    assert(inbox_match(db, sid, "system", "is paused") == 1);
    assert(inbox_match(db, sid, "system", "the model refused") == 1);
    assert(inbox_match(db, sid, "cron", "go") == 0);      /* the fire was refused */

    /* A success in the window resets it: re-enabled, the job fires again. */
    stage_fire(db, sid, "broken", 0);
    exec_ok(db, "UPDATE cron_jobs SET enabled=1, next_run_at=%lld",
            (long long)(now - 1));
    cron_run_due(db);
    assert(scalar(db, q) == 1);
    assert(inbox_match(db, sid, "cron", "go") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: three failed fires auto-pause, a success resets\n");
}

/* ── Script payload: the cron.c ↔ daemon contract ─────────────────────
 * The dispatcher itself lives in main.c (it needs the child table); what cron.c
 * owns is the decision it hands over. A stub runner is how that hand-off gets
 * tested without forking anything. */
static struct {
    int calls;
    char job[64], script[128], agent[64], prompt[128];
    int64_t session_id;
    CronScriptRc rc;
} g_stub;

static CronScriptRc stub_runner(const CronScriptFire *f, char *err, size_t n) {
    g_stub.calls++;
    snprintf(g_stub.job, sizeof(g_stub.job), "%s", f->job_name ? f->job_name : "");
    snprintf(g_stub.script, sizeof(g_stub.script), "%s", f->script ? f->script : "");
    snprintf(g_stub.agent, sizeof(g_stub.agent), "%s", f->agent_name ? f->agent_name : "");
    snprintf(g_stub.prompt, sizeof(g_stub.prompt), "%s", f->prompt ? f->prompt : "");
    g_stub.session_id = f->session_id;
    if (g_stub.rc == CRON_SCRIPT_FAILED) snprintf(err, n, "script is missing");
    return g_stub.rc;
}

static void test_fire_script_handoff(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);
    memset(&g_stub, 0, sizeof(g_stub));
    cron_set_script_runner(stub_runner);

    /* script only: no inbox row — the result posts when the child finishes. */
    JobSpec j = {.agent = "A", .name = "scan", .interval_s = 600,
                 .session_id = sid, .script = "jobs/scan.qjs",
                 .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);
    assert(g_stub.calls == 1);
    assert(strcmp(g_stub.job, "scan") == 0);
    assert(strcmp(g_stub.script, "jobs/scan.qjs") == 0);
    assert(strcmp(g_stub.agent, "A") == 0);
    assert(g_stub.session_id == sid);
    assert(g_stub.prompt[0] == '\0');
    assert(inbox_match(db, sid, NULL, NULL) == 0);

    /* both: the prompt rides along so one user entry carries prompt + output. */
    exec_ok(db, "UPDATE cron_jobs SET task='look at this', next_run_at=%lld"
                " WHERE id=%lld", (long long)(now - 1), (long long)jid);
    cron_run_due(db);
    assert(g_stub.calls == 2);
    assert(strcmp(g_stub.prompt, "look at this") == 0);
    assert(inbox_match(db, sid, NULL, NULL) == 0);   /* still nothing until it finishes */

    /* A refused spawn is not a failed fire — the next schedule retries. */
    g_stub.rc = CRON_SCRIPT_BUSY;
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);
    assert(g_stub.calls == 3);
    assert(inbox_match(db, sid, "cron_error", NULL) == 0);

    /* A script that cannot run at all is, and it says why. */
    g_stub.rc = CRON_SCRIPT_FAILED;
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);
    assert(inbox_match(db, sid, "cron_error", "script is missing") == 1);

    cron_set_script_runner(NULL);
    wake_close();
    db_close(db);
    printf("  PASS: script fires hand the daemon job/script/agent/session\n");
}

/* Outside the daemon there is no dispatcher, and a script job must say so
 * rather than fail silently. */
static void test_fire_script_without_runner(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "A", .name = "scan", .interval_s = 600,
                 .session_id = sid, .script = "jobs/scan.qjs",
                 .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);
    assert(inbox_match(db, sid, "cron_error", "daemon only") == 1);

    wake_close();
    db_close(db);
    printf("  PASS: a script fire with no dispatcher reports the refusal\n");
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
    test_cron_schedule_validation();
    test_cron_floor_fire_to_fire();
    test_cron_schedule_check_codes();
    test_dispatch_oneshot();
    test_dispatch_interval();
    test_dispatch_session_zero_resolves();
    test_dispatch_session_zero_no_session();
    test_seed_heartbeat_shape();
    test_fire_bare_wake_nudges_session();
    test_fire_bare_wake_busy_session();
    test_fire_bare_wake_no_stack();
    test_fire_stamps_source_ref();
    test_fire_follows_route();
    test_fire_route_gone_falls_back();
    test_fire_authority_lost();
    test_fire_pin_gone_errors();
    test_fire_new_session_chat();
    test_fire_new_session_parented();
    test_fire_new_session_no_depth_chain();
    test_upsert_new_seeds_no_interlock();
    test_fire_coalesces();
    test_fire_skip_if_busy();
    test_fire_bare_wake();
    test_fire_failure_auto_pause();
    test_fire_script_handoff();
    test_fire_script_without_runner();
    printf("ALL PASSED\n");
    return 0;
}
