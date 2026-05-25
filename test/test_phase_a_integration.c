/* T61: Phase A Integration — verification matrices for overlap rejection (V16,V19) */
#include "db.h"
#include "janitor.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

static const char *DB_PATH = "/tmp/test_cclaw_phase_a.sqlite";

/* Race result shared between threads */
typedef struct {
    sqlite3 *db;
    int64_t session_id;
    const char *holder;
    int result;
} AcquireArg;

static void *thread_acquire(void *arg) {
    AcquireArg *a = (AcquireArg *)arg;
    a->result = session_try_acquire(a->db, a->session_id, a->holder);
    return NULL;
}

/* Matrix 1: Two threads race to acquire same session — exactly one wins */
static void test_race_only_one_wins(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "race_test", NULL, -1, 0);
    assert(sid > 0);

    AcquireArg a1 = {.db = db, .session_id = sid, .holder = "worker-A"};
    AcquireArg a2 = {.db = db, .session_id = sid, .holder = "worker-B"};

    pthread_t t1, t2;
    pthread_create(&t1, NULL, thread_acquire, &a1);
    pthread_create(&t2, NULL, thread_acquire, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);

    /* Exactly one succeeds (0), one fails (-1) */
    assert((a1.result == 0 && a2.result == -1) ||
           (a1.result == -1 && a2.result == 0));

    /* Release winner */
    const char *winner = (a1.result == 0) ? "worker-A" : "worker-B";
    assert(session_release(db, sid, winner) == 0);

    db_close(db);
    printf("  PASS test_race_only_one_wins\n");
}

/* Matrix 2: Acquire while running → rejected (no overlap) */
static void test_running_rejects_all(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "overlap_test", NULL, -1, 0);
    assert(sid > 0);

    assert(session_try_acquire(db, sid, "cli-worker") == 0);

    /* Telegram worker, sub-agent worker, another CLI — all rejected */
    assert(session_try_acquire(db, sid, "telegram-poller") == -1);
    assert(session_try_acquire(db, sid, "sub-agent-1") == -1);
    assert(session_try_acquire(db, sid, "cli-worker-2") == -1);

    assert(session_release(db, sid, "cli-worker") == 0);
    db_close(db);
    printf("  PASS test_running_rejects_all\n");
}

/* Matrix 3: Janitor does NOT steal fresh (non-stale) locks */
static void test_janitor_respects_active_locks(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "janitor_safe", NULL, -1, 0);
    assert(sid > 0);

    assert(session_try_acquire(db, sid, "active-worker") == 0);

    /* Sweep with generous timeout — should not touch fresh lock */
    int recovered = janitor_sweep(db, 300);
    assert(recovered == 0);

    /* Lock still held — others still rejected */
    assert(session_try_acquire(db, sid, "intruder") == -1);

    assert(session_release(db, sid, "active-worker") == 0);
    db_close(db);
    printf("  PASS test_janitor_respects_active_locks\n");
}

/* Matrix 4: Independent sessions don't interfere */
static void test_independent_sessions_no_interference(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t s1 = session_create(db, "session-A", NULL, -1, 0);
    int64_t s2 = session_create(db, "session-B", NULL, -1, 0);
    int64_t s3 = session_create(db, "session-C", NULL, -1, 0);
    assert(s1 > 0 && s2 > 0 && s3 > 0);

    /* All three can be acquired simultaneously by different workers */
    assert(session_try_acquire(db, s1, "cli") == 0);
    assert(session_try_acquire(db, s2, "telegram") == 0);
    assert(session_try_acquire(db, s3, "sub-agent") == 0);

    /* But same session still rejects overlap */
    assert(session_try_acquire(db, s1, "other") == -1);
    assert(session_try_acquire(db, s2, "other") == -1);

    assert(session_release(db, s1, "cli") == 0);
    assert(session_release(db, s2, "telegram") == 0);
    assert(session_release(db, s3, "sub-agent") == 0);

    db_close(db);
    printf("  PASS test_independent_sessions_no_interference\n");
}

/* Matrix 5: Release then re-acquire by different worker succeeds */
static void test_handoff_between_workers(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "handoff", NULL, -1, 0);
    assert(sid > 0);

    /* CLI acquires, processes, releases */
    assert(session_try_acquire(db, sid, "cli") == 0);
    assert(session_release(db, sid, "cli") == 0);

    /* Telegram picks up same session */
    assert(session_try_acquire(db, sid, "telegram") == 0);
    assert(session_release(db, sid, "telegram") == 0);

    /* Sub-agent picks up */
    assert(session_try_acquire(db, sid, "sub-agent") == 0);
    assert(session_release(db, sid, "sub-agent") == 0);

    db_close(db);
    printf("  PASS test_handoff_between_workers\n");
}

/* Matrix 6: Many threads racing same session — exactly one winner */
static void test_many_threads_one_winner(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "many_race", NULL, -1, 0);
    assert(sid > 0);

    #define N_THREADS 8
    pthread_t threads[N_THREADS];
    AcquireArg args[N_THREADS];
    char names[N_THREADS][32];

    for (int i = 0; i < N_THREADS; i++) {
        snprintf(names[i], sizeof(names[i]), "worker-%d", i);
        args[i] = (AcquireArg){.db = db, .session_id = sid, .holder = names[i]};
        pthread_create(&threads[i], NULL, thread_acquire, &args[i]);
    }
    for (int i = 0; i < N_THREADS; i++)
        pthread_join(threads[i], NULL);

    int winners = 0;
    const char *winner_name = NULL;
    for (int i = 0; i < N_THREADS; i++) {
        if (args[i].result == 0) {
            winners++;
            winner_name = args[i].holder;
        }
    }
    assert(winners == 1);
    assert(winner_name != NULL);
    assert(session_release(db, sid, winner_name) == 0);

    db_close(db);
    printf("  PASS test_many_threads_one_winner\n");
    #undef N_THREADS
}

int main(void) {
    unlink(DB_PATH);
    printf("test_phase_a_integration:\n");
    test_race_only_one_wins();
    test_running_rejects_all();
    test_janitor_respects_active_locks();
    test_independent_sessions_no_interference();
    test_handoff_between_workers();
    test_many_threads_one_winner();
    unlink(DB_PATH);
    printf("All phase A integration tests passed.\n");
    return 0;
}
