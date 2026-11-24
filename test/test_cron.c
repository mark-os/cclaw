#define _GNU_SOURCE
#include "config_registry.h"
#include "cron.h"
#include "db.h"
#include "wake.h"
#include "test_util.h"
#include "channel.h"
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
        "agent_a", "agent_b", "A", "Hb",
        "Chat", "Other", "New", "Fail", NULL
    };
    for (int i = 0; agents[i]; i++)
        test_seed_agent(db, agents[i]);
    return db;
}

/* A fire-path test: in-memory DB plus the wake pipe cron_run_due nudges. */
static sqlite3 *fire_fixture(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db && wake_init() == 0);
    return db;
}

static void fire_fixture_done(sqlite3 *db) {
    wake_close();
    db_close(db);
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

/* Fields evaluate in the daemon's LOCAL timezone, not UTC. Zones are POSIX
 * TZ strings rather than "America/New_York" so the test needs no tzdata on
 * the box and can't drift when tzdata ships new rules. */
#define TZ_UTC     "UTC0"
#define TZ_EASTERN "EST5EDT,M3.2.0/2,M11.1.0/2"

static void set_tz(const char *tz) {
    if (tz) setenv("TZ", tz, 1);
    else unsetenv("TZ");
    tzset();
}

static void test_cron_next_run(void) {
    static const struct {
        const char *expr, *tz;
        int64_t base, want;     /* want -1: refused */
    } cases[] = {
        { "* * * * *",    TZ_UTC, 1700000000, 1700000040 },
        { "30 2 * * *",   TZ_UTC, 1705280400, 1705285800 },  /* 2024-01-15 02:30 */
        { "*/15 * * * *", TZ_UTC, 1700000000, 1700000100 },
        { NULL,           TZ_UTC, 0, -1 },
        { "",             TZ_UTC, 0, -1 },
        { "bad",          TZ_UTC, 0, -1 },
        { "60 * * * *",   TZ_UTC, 0, -1 },
        /* 07:00 EST; 9am local is 14:00 UTC the same day (under gmtime_r it
         * was 09:00 UTC, already past, and landed on the 16th). */
        { "0 9 * * *",  TZ_EASTERN, 1705320000, 1705327200 },
        /* Spring forward 2024-03-10: 9am EST on the 9th to 9am EDT on the
         * 10th is a 23-hour day. */
        { "0 9 * * *",  TZ_EASTERN, 1709992800, 1710075600 },
        /* Fall back 2024-11-03: a 25-hour day. */
        { "0 10 * * *", TZ_EASTERN, 1730556000, 1730646000 },
        /* 02:30 does not exist on 2024-03-10, so the next 02:30 is the 11th. */
        { "30 2 * * *", TZ_EASTERN, 1710046800, 1710138600 },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        set_tz(cases[i].tz);
        int64_t got = cron_next_run(cases[i].expr, cases[i].base);
        if (got != cases[i].want) {
            fprintf(stderr, "row %zu (%s): got %lld want %lld\n", i,
                    cases[i].expr ? cases[i].expr : "NULL",
                    (long long)got, (long long)cases[i].want);
            assert(0);
        }
    }
    set_tz(NULL);
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

/* Agent scoping (review-2 F2): one agent's jobs are invisible to another's
 * list and cannot be removed by it. */
static void test_cron_agent_scoping(void) {
    sqlite3 *db = open_seeded(":memory:");
    assert(db);
    int64_t sid = session_create(db, "cron_test", NULL, -1, 0);
    assert(sid > 0);

    int64_t jid = add_recurring(db, "agent_a", "job_a", "0 * * * *", sid, "task_a");
    assert(jid > 0);
    assert(add_recurring(db, "agent_b", "job_b", "0 * * * *", sid, "task_b") > 0);
    assert(add_recurring(db, "agent_a", "bad", "invalid", sid, "x") == -1);

    int count = 0;
    CronJob *jobs = cron_list(db, "agent_a", &count);
    assert(count == 1 && strcmp(jobs[0].name, "job_a") == 0);
    cron_list_free(jobs, count);
    jobs = cron_list(db, "nonexistent", &count);
    assert(count == 0 && jobs == NULL);

    assert(cron_remove(db, jid, "agent_b") == -1);
    jobs = cron_list(db, "agent_a", &count);
    assert(count == 1);
    cron_list_free(jobs, count);
    assert(cron_remove(db, jid, "agent_a") == 0);

    db_close(db);
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
    return db_scalar_i64(db, "SELECT COUNT(*) FROM cron_jobs WHERE id=?1", id, -1);
}

static int64_t job_enabled(sqlite3 *db, int64_t id) {
    return db_scalar_i64(db, "SELECT enabled FROM cron_jobs WHERE id=?1", id, -1);
}

static int64_t job_next(sqlite3 *db, int64_t id) {
    return db_scalar_i64(db, "SELECT next_run_at FROM cron_jobs WHERE id=?1", id, -1);
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
    sqlite3 *db = fire_fixture();
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    int64_t jid = insert_due(db, "A", "", now, 0, sid, "ping", now);
    cron_run_due(db);

    assert(inbox_match(db, sid, "cron", "ping") == 1);
    assert(job_count(db, jid) == 0);   /* one-shot self-deletes */

    /* Fire again: nothing to re-fire, still exactly one inbox row. */
    cron_run_due(db);
    assert(inbox_match(db, sid, "cron", "ping") == 1);

    fire_fixture_done(db);
}

static void test_dispatch_interval(void) {
    sqlite3 *db = fire_fixture();
    int64_t sid = session_create(db, "s", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    int64_t jid = insert_due(db, "A", "", 0, 600, sid, "tick", now - 1);
    cron_run_due(db);

    assert(inbox_match(db, sid, "cron", "tick") == 1);
    assert(job_count(db, jid) == 1);              /* persists */
    assert(job_next(db, jid) >= now + 600 - 2);   /* advanced by interval */

    fire_fixture_done(db);
}

/* ── The agent pulse: an ordinary bare-wake job ─────────────────────── */

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

/* session_id=0 late-resolves to the agent's most recent session, and a bare
 * wake adds nothing of its own — it just gets that session moving on what is
 * already queued there. */
static void test_fire_bare_wake_nudges_session(void) {
    sqlite3 *db = fire_fixture();
    int64_t sid = session_create(db, "s", "Hb", -1, 0);
    inbox_insert(db, sid, "channel", NULL, "queued while asleep");
    int64_t now = (int64_t)time(NULL);
    wake_drain(NULL);

    insert_due(db, "Hb", "*/30 * * * *", 0, 0, 0, "", now - 1);
    cron_run_due(db);

    int64_t woke = 0;
    assert(wake_drain(&woke) == 1 && woke == sid);
    assert(inbox_match(db, sid, NULL, NULL) == 1);   /* only the queued row */

    fire_fixture_done(db);
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
    if (j->channel && j->chat) test_seed_chat(db, j->channel, j->chat);
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
    test_seed_chat(db, chan, chat);
    assert(channel_pin_session(db, chan, chat, sid) == 0);
}

/* source_ref carries the job NAME, not its id: ids die with one-shot removal
 * and delete-recreate cycles, names are the logical identity. */
static void test_fire_stamps_source_ref(void) {
    sqlite3 *db = fire_fixture();
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

    fire_fixture_done(db);
}

/* Follow-the-conversation: the chat is the durable identity, so the fire goes
 * to whatever session is routed there NOW — not the one stamped at set time. */
static void test_fire_follows_route(void) {
    sqlite3 *db = fire_fixture();
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

    fire_fixture_done(db);
}

/* No route for that chat any more (it was never re-pinned, or the row was
 * dropped): the stamped session is the fallback, not an error. */
static void test_fire_route_gone_falls_back(void) {
    sqlite3 *db = fire_fixture();
    int64_t stamped = session_create(db, "old", "Chat", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "Chat", .name = "follow", .interval_s = 600,
                 .session_id = stamped, .task = "ping", .channel = "tg",
                 .chat = "42", .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    assert(inbox_match(db, stamped, "cron", "ping") == 1);

    fire_fixture_done(db);
}

/* Authority is re-checked at fire time: a chat re-routed to another agent is
 * no longer reachable through a standing job. */
static void test_fire_authority_lost(void) {
    sqlite3 *db = fire_fixture();
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

    fire_fixture_done(db);
}

/* A pin is stale-by-choice — but a vanished session is reported, not silent. */
static void test_fire_pin_gone_errors(void) {
    sqlite3 *db = fire_fixture();
    int64_t owner = session_create(db, "own", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "A", .name = "pinned", .interval_s = 600,
                 .session_id = 99999, .task = "ping", .target = "pin",
                 .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    assert(inbox_match(db, owner, "cron_error", "pinned session") == 1);

    fire_fixture_done(db);
}

/* session:"new" with channel fields: a fresh session per fire, stamped with
 * the chat so ordinary delivery reaches it. */
static void test_fire_new_session_chat(void) {
    sqlite3 *db = fire_fixture();
    int64_t bound = session_create(db, "bound", "Chat", -1, 0);
    route_pin(db, "tg", "42", bound);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "Chat", .name = "digest", .interval_s = 600,
                 .task = "summarize", .target = "new", .channel = "tg",
                 .chat = "42", .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);

    int64_t fresh = test_qi(db, "SELECT id FROM sessions WHERE name='cron'");
    assert(fresh > 0 && fresh != bound);
    assert(inbox_match(db, fresh, "cron", "summarize") == 1);
    assert(inbox_match(db, bound, "cron", "summarize") == 0);
    assert(test_qi(db, "SELECT chat_id='42' AND channel_name='tg' FROM sessions"
                      " WHERE name='cron'") == 1);
    /* the fired session is recorded — skip-if-busy reads it next time */
    assert(db_scalar_i64(db, "SELECT session_id FROM cron_jobs WHERE id=?1", jid, -1) == fresh);

    fire_fixture_done(db);
}

/* session:"new" without channel fields: parented to the late-resolved owner,
 * so the result rides the existing notify_parent push. */
static void test_fire_new_session_parented(void) {
    sqlite3 *db = fire_fixture();
    int64_t owner = session_create(db, "own", "New", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "New", .name = "checkin", .interval_s = 600,
                 .task = "status?", .target = "new", .next_run_at = now - 1};
    insert_job(db, &j);
    cron_run_due(db);

    int64_t fresh = test_qi(db, "SELECT id FROM sessions WHERE name='cron'");
    assert(fresh > 0);
    assert(inbox_match(db, fresh, "cron", "status?") == 1);
    assert(db_scalar_i64(db, "SELECT parent_session_id FROM sessions"
                             " WHERE id=?1 AND parent_tool_call_id IS NULL", fresh, -1) == owner);

    fire_fixture_done(db);
}

/* Recurring 'new' fires must not chain: each fresh session parents to the
 * owner, not to the previous fire's session — chaining walks depth up to
 * agent_max_depth and locks launch_agent out by the second fire. */
static void test_fire_new_session_no_depth_chain(void) {
    sqlite3 *db = fire_fixture();
    int64_t owner = session_create(db, "own", "New", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "New", .name = "daily", .interval_s = 600,
                 .task = "report", .target = "new", .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);
    int64_t first = test_qi(db, "SELECT id FROM sessions WHERE name='cron'");
    assert(first > 0);
    /* First fire done and idle; owner deliberately stays older than it. */
    exec_ok(db, "UPDATE inbox SET consumed=1 WHERE session_id=%lld",
            (long long)first);
    exec_ok(db, "UPDATE sessions SET updated_at=updated_at+10 WHERE id=%lld",
            (long long)first);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld WHERE id=%lld",
            (long long)(now - 1), (long long)jid);
    cron_run_due(db);
    char q[192];
    snprintf(q, sizeof(q),
             "SELECT parent_session_id=%lld AND depth=1 FROM sessions"
             " WHERE name='cron' AND id<>%lld", (long long)owner, (long long)first);
    assert(test_qi(db, q) == 1);

    fire_fixture_done(db);
}

/* Setting a 'new' job must not seed skip-if-busy's interlock with the
 * caller's chat session — that would skip the first fires whenever the human
 * who set the job is mid-conversation. */
static void test_upsert_new_seeds_no_interlock(void) {
    sqlite3 *db = fire_fixture();
    int64_t caller = session_create(db, "chat", "New", -1, 0);
    assert(cron_upsert(db, "New",  caller,
        "{\"name\":\"digest\",\"cron_expr\":\"0 * * * *\",\"prompt\":\"go\","
        "\"session\":\"new\"}", NULL, NULL) > 0);
    assert(test_qi(db, "SELECT session_id FROM cron_jobs WHERE name='digest'") == 0);

    /* But re-setting an already-'new' job keeps the previous fire's stamp. */
    exec_ok(db, "UPDATE cron_jobs SET session_id=%lld WHERE name='digest'",
            (long long)caller);
    assert(cron_upsert(db, "New", caller,
        "{\"name\":\"digest\",\"prompt\":\"go faster\"}", NULL, NULL) > 0);
    assert(test_qi(db, "SELECT session_id FROM cron_jobs WHERE name='digest'") == caller);

    fire_fixture_done(db);
}

/* One outstanding fire per job: the second tick coalesces onto the first, and
 * only a drain unblocks the next one. */
static void test_fire_coalesces(void) {
    sqlite3 *db = fire_fixture();
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

    fire_fixture_done(db);
}

/* Skip-if-busy covers what coalescing cannot: a 'new' fire targets a session
 * that did not exist yet, so the previous fire's session is the interlock. */
static void test_fire_skip_if_busy(void) {
    sqlite3 *db = fire_fixture();
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
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 0);

    /* Created but never started — idle with its prompt still queued — is just
     * as live: firing again would stack sessions behind whatever holds it. */
    exec_ok(db, "UPDATE sessions SET state='idle' WHERE id=%lld", (long long)busy);
    assert(inbox_insert(db, busy, "cron", "digest", "again") > 0);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 0);

    /* Once it has drained and gone idle the next fire proceeds. */
    exec_ok(db, "UPDATE inbox SET consumed=1 WHERE session_id=%lld", (long long)busy);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 1);

    fire_fixture_done(db);
}

/* The launch gate (rule 1): a 'new' fire is a producer creating a session, so
 * over session_max_active it skips + logs — no session, no cron_error (a skip
 * must not feed the 3-strike auto-pause) — and the next schedule retries. */
static void test_fire_new_skips_over_session_max_active(void) {
    sqlite3 *db = fire_fixture();
    assert(config_set(db, "session_max_active", "1") == 0);
    int64_t now = (int64_t)time(NULL);

    /* The fleet is full: one in-flight sub-agent session at cap 1. */
    int64_t parent = session_create(db, "own", "New", -1, 0);
    int64_t sub = session_create(db, "worker", "New", parent, 1);
    exec_ok(db, "UPDATE sessions SET state='llm_running' WHERE id=%lld",
            (long long)sub);

    JobSpec j = {.agent = "New", .name = "report", .interval_s = 600,
                 .task = "daily digest", .target = "new",
                 .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 0);
    assert(test_qi(db, "SELECT COUNT(*) FROM inbox") == 0);   /* no cron_error */
    assert(job_enabled(db, jid) == 1);
    assert(job_next(db, jid) >= now + 600 - 2);              /* rescheduled */

    /* A slot frees: the next due fire creates its session as usual. */
    exec_ok(db, "UPDATE sessions SET state='idle' WHERE id=%lld", (long long)sub);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 1);

    fire_fixture_done(db);
}

/* Neither prompt nor script: advance the session, annotate nothing. */
static void test_fire_bare_wake(void) {
    sqlite3 *db = fire_fixture();
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
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 0);

    fire_fixture_done(db);
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
    sqlite3 *db = fire_fixture();
    int64_t sid = session_create(db, "s", "Fail", -1, 0);
    int64_t now = (int64_t)time(NULL);

    JobSpec j = {.agent = "Fail", .name = "broken", .interval_s = 600,
                 .session_id = sid, .task = "go", .next_run_at = now - 1};
    int64_t jid = insert_job(db, &j);

    /* Two failures is not yet a streak — even when a hook rewrote the second
     * prompt: the stand-in keeps the cron's source, and is the same fire. */
    stage_fire(db, sid, "broken", 1);
    stage_fire(db, sid, "broken", 1);
    exec_ok(db, "INSERT INTO entries(session_id,parent_id,turn_id,type,role,content,data)"
                " SELECT session_id,-1,turn_id,type,role,'[cron: broken] go, briefly',"
                "        json_set(data,'$.edited_by','hook:x','$.replaces',id)"
                " FROM entries WHERE id=(SELECT MAX(id) FROM entries WHERE role=1)");
    cron_run_due(db);
    assert(job_enabled(db, jid) == 1);
    assert(inbox_match(db, sid, "cron", "go") == 1);      /* still firing */
    assert(inbox_consume_into_entries(db, sid, 10) == 1);

    /* The third trips it: enabled=0, and the owner is told, with the error. */
    stage_fire(db, sid, "broken", 1);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)(now - 1));
    cron_run_due(db);
    assert(job_enabled(db, jid) == 0);
    assert(inbox_match(db, sid, "system", "is paused") == 1);
    assert(inbox_match(db, sid, "system", "the model refused") == 1);
    assert(inbox_match(db, sid, "cron", "go") == 0);      /* the fire was refused */

    /* A success in the window resets it: re-enabled, the job fires again. */
    stage_fire(db, sid, "broken", 0);
    exec_ok(db, "UPDATE cron_jobs SET enabled=1, next_run_at=%lld",
            (long long)(now - 1));
    cron_run_due(db);
    assert(job_enabled(db, jid) == 1);
    assert(inbox_match(db, sid, "cron", "go") == 1);

    fire_fixture_done(db);
}

/* A one-shot has no next schedule to retry on, so a fire that only ran into a
 * limit must not delete it: it stays armed and goes when there is room. */
static void test_oneshot_deferred_stays_armed(void) {
    sqlite3 *db = fire_fixture();
    assert(config_set(db, "session_max_active", "1") == 0);
    int64_t now = (int64_t)time(NULL);
    int64_t parent = session_create(db, "own", "New", -1, 0);
    int64_t sub = session_create(db, "worker", "New", parent, 1);
    exec_ok(db, "UPDATE sessions SET state='llm_running' WHERE id=%lld",
            (long long)sub);

    JobSpec j = {.agent = "New", .name = "remind", .run_at = now,
                 .task = "call back", .target = "new", .next_run_at = now};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);
    assert(job_count(db, jid) == 1);                         /* still armed */
    assert(job_next(db, jid) > now);                         /* not every tick */
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 0);
    assert(test_qi(db, "SELECT COUNT(*) FROM inbox") == 0);   /* not an error */

    exec_ok(db, "UPDATE sessions SET state='idle' WHERE id=%lld", (long long)sub);
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)now);
    cron_run_due(db);
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 1);
    assert(job_count(db, jid) == 0);                         /* fired, gone */

    fire_fixture_done(db);
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
    sqlite3 *db = fire_fixture();
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
    fire_fixture_done(db);
}

/* No child slot for a 'new' script fire: the session made for it must not be
 * left behind, or every retry would add another empty one. */
static void test_fire_script_busy_leaves_no_session(void) {
    sqlite3 *db = fire_fixture();
    session_create(db, "own", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);
    memset(&g_stub, 0, sizeof(g_stub));
    g_stub.rc = CRON_SCRIPT_BUSY;
    cron_set_script_runner(stub_runner);

    JobSpec j = {.agent = "A", .name = "scan", .run_at = now, .target = "new",
                 .script = "jobs/scan.qjs", .next_run_at = now};
    int64_t jid = insert_job(db, &j);
    cron_run_due(db);
    assert(g_stub.calls == 1);
    assert(job_count(db, jid) == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 0);
    assert(test_qi(db, "SELECT COUNT(*) FROM delivery_edges") == 0);

    g_stub.rc = CRON_SCRIPT_SPAWNED;
    exec_ok(db, "UPDATE cron_jobs SET next_run_at=%lld", (long long)now);
    cron_run_due(db);
    assert(test_qi(db, "SELECT COUNT(*) FROM sessions WHERE name='cron'") == 1);
    assert(job_count(db, jid) == 0);

    cron_set_script_runner(NULL);
    fire_fixture_done(db);
}

int main(void) {
    TEST_INIT();
    printf("test_cron:\n");
    RUN(test_cron_next_run);
    RUN(test_cron_agent_scoping);
    RUN(test_cron_schedule_validation);
    RUN(test_cron_floor_fire_to_fire);
    RUN(test_cron_schedule_check_codes);
    RUN(test_dispatch_oneshot);
    RUN(test_dispatch_interval);
    RUN(test_fire_bare_wake_nudges_session);
    RUN(test_fire_stamps_source_ref);
    RUN(test_fire_follows_route);
    RUN(test_fire_route_gone_falls_back);
    RUN(test_fire_authority_lost);
    RUN(test_fire_pin_gone_errors);
    RUN(test_fire_new_session_chat);
    RUN(test_fire_new_session_parented);
    RUN(test_fire_new_session_no_depth_chain);
    RUN(test_upsert_new_seeds_no_interlock);
    RUN(test_fire_coalesces);
    RUN(test_fire_skip_if_busy);
    RUN(test_fire_new_skips_over_session_max_active);
    RUN(test_oneshot_deferred_stays_armed);
    RUN(test_fire_script_busy_leaves_no_session);
    RUN(test_fire_bare_wake);
    RUN(test_fire_failure_auto_pause);
    RUN(test_fire_script_handoff);
    return 0;
}
