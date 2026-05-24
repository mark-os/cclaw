/* T70: Integration Test — parallel high-throughput network payloads (V16, V18)
 * Verifies CAS locking and inbox atomicity under concurrent pressure. */
#define _POSIX_C_SOURCE 199309L
#include "db.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

static const char *DB_PATH = "/tmp/test_cclaw_parallel_throughput.sqlite";

#define N_PRODUCERS 8
#define MSGS_PER_PRODUCER 50
#define N_CONSUMERS 4
#define TOTAL_MSGS (N_PRODUCERS * MSGS_PER_PRODUCER)

typedef struct {
    const char *db_path;
    int64_t session_id;
    int producer_id;
} ProducerArg;

typedef struct {
    const char *db_path;
    int64_t session_id;
    int consumer_id;
    int consumed;
} ConsumerArg;

/* Producer: open own DB connection, insert MSGS_PER_PRODUCER inbox items */
static void *producer_thread(void *arg) {
    ProducerArg *p = (ProducerArg *)arg;
    sqlite3 *db = db_open(p->db_path);
    assert(db);

    for (int i = 0; i < MSGS_PER_PRODUCER; i++) {
        char payload[64];
        snprintf(payload, sizeof(payload), "p%d_msg%d", p->producer_id, i);
        int64_t r = inbox_insert(db, p->session_id, "network", payload);
        assert(r > 0);
    }

    db_close(db);
    return NULL;
}

/* Consumer: repeatedly try to acquire session, consume inbox, release */
static void *consumer_thread(void *arg) {
    ConsumerArg *c = (ConsumerArg *)arg;
    sqlite3 *db = db_open(c->db_path);
    assert(db);
    c->consumed = 0;

    char holder[32];
    snprintf(holder, sizeof(holder), "consumer-%d", c->consumer_id);

    for (int attempt = 0; attempt < 200; attempt++) {
        if (session_try_acquire(db, c->session_id, holder) == 0) {
            int n = inbox_consume_into_entries(db, c->session_id, 10);
            if (n > 0) c->consumed += n;
            session_release(db, c->session_id, holder);
        }
        /* Small yield to let others run */
        struct timespec ts = {0, 500000}; /* 0.5ms */
        nanosleep(&ts, NULL);
    }

    db_close(db);
    return NULL;
}

/* Test: many producers + many consumers racing — all messages consumed exactly once */
static void test_parallel_produce_consume(void) {
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "throughput", NULL);
    assert(sid > 0);
    db_close(db);

    /* Launch producers */
    pthread_t prod_threads[N_PRODUCERS];
    ProducerArg prod_args[N_PRODUCERS];
    for (int i = 0; i < N_PRODUCERS; i++) {
        prod_args[i] = (ProducerArg){.db_path = DB_PATH, .session_id = sid, .producer_id = i};
        pthread_create(&prod_threads[i], NULL, producer_thread, &prod_args[i]);
    }
    /* Wait for all producers to finish */
    for (int i = 0; i < N_PRODUCERS; i++)
        pthread_join(prod_threads[i], NULL);

    /* Launch consumers */
    pthread_t cons_threads[N_CONSUMERS];
    ConsumerArg cons_args[N_CONSUMERS];
    for (int i = 0; i < N_CONSUMERS; i++) {
        cons_args[i] = (ConsumerArg){.db_path = DB_PATH, .session_id = sid, .consumer_id = i};
        pthread_create(&cons_threads[i], NULL, consumer_thread, &cons_args[i]);
    }
    for (int i = 0; i < N_CONSUMERS; i++)
        pthread_join(cons_threads[i], NULL);

    /* Verify: total consumed across all consumers == TOTAL_MSGS */
    int total_consumed = 0;
    for (int i = 0; i < N_CONSUMERS; i++)
        total_consumed += cons_args[i].consumed;
    assert(total_consumed == TOTAL_MSGS);

    /* Verify: no unconsumed inbox items remain */
    db = db_open(DB_PATH);
    int remaining = 0;
    InboxItem *items = inbox_peek(db, sid, TOTAL_MSGS + 1, &remaining);
    assert(remaining == 0);
    assert(items == NULL);

    /* Verify: exactly TOTAL_MSGS entries in session branch */
    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(ecount == TOTAL_MSGS);
    entry_branch_free(entries, ecount);

    db_close(db);
    printf("  PASS test_parallel_produce_consume\n");
}

/* Test: concurrent producers + consumers interleaved (simultaneous insert & consume) */
static void test_interleaved_produce_consume(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "interleaved", NULL);
    assert(sid > 0);
    db_close(db);

    /* Launch producers and consumers simultaneously */
    pthread_t prod_threads[N_PRODUCERS];
    pthread_t cons_threads[N_CONSUMERS];
    ProducerArg prod_args[N_PRODUCERS];
    ConsumerArg cons_args[N_CONSUMERS];

    for (int i = 0; i < N_PRODUCERS; i++) {
        prod_args[i] = (ProducerArg){.db_path = DB_PATH, .session_id = sid, .producer_id = i};
        pthread_create(&prod_threads[i], NULL, producer_thread, &prod_args[i]);
    }
    for (int i = 0; i < N_CONSUMERS; i++) {
        cons_args[i] = (ConsumerArg){.db_path = DB_PATH, .session_id = sid, .consumer_id = i};
        pthread_create(&cons_threads[i], NULL, consumer_thread, &cons_args[i]);
    }

    for (int i = 0; i < N_PRODUCERS; i++)
        pthread_join(prod_threads[i], NULL);
    for (int i = 0; i < N_CONSUMERS; i++)
        pthread_join(cons_threads[i], NULL);

    /* Some messages may not be consumed yet (consumers may finish before producers).
     * Drain remaining with a final consume pass. */
    db = db_open(DB_PATH);
    int extra = 0;
    int total_consumed = 0;
    for (int i = 0; i < N_CONSUMERS; i++)
        total_consumed += cons_args[i].consumed;

    /* Consume any stragglers */
    if (total_consumed < TOTAL_MSGS) {
        assert(session_try_acquire(db, sid, "drain") == 0);
        extra = inbox_consume_into_entries(db, sid, TOTAL_MSGS);
        if (extra < 0) extra = 0;
        session_release(db, sid, "drain");
        total_consumed += extra;
    }
    assert(total_consumed == TOTAL_MSGS);

    /* No unconsumed items remain */
    int remaining = 0;
    InboxItem *items = inbox_peek(db, sid, 1, &remaining);
    assert(remaining == 0);
    assert(items == NULL);

    /* Entry count matches */
    int ecount = 0;
    Entry *entries = session_get_branch(db, sid, &ecount);
    assert(ecount == TOTAL_MSGS);
    entry_branch_free(entries, ecount);

    db_close(db);
    printf("  PASS test_interleaved_produce_consume\n");
}

/* Test: CAS prevents overlapping consumers on same session (V16) */
static void test_cas_no_overlap_under_load(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    int64_t sid = session_create(db, "cas_load", NULL);
    assert(sid > 0);

    /* Pre-fill inbox */
    for (int i = 0; i < 100; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "item_%d", i);
        inbox_insert(db, sid, "net", buf);
    }
    db_close(db);

    /* Track concurrent holders — should never exceed 1 */
    static volatile int active_holders = 0;
    static volatile int max_concurrent = 0;
    static pthread_mutex_t counter_lock = PTHREAD_MUTEX_INITIALIZER;

    /* We need a custom consumer that checks concurrency */
    typedef struct {
        const char *db_path;
        int64_t session_id;
        int id;
        int consumed;
    } CASConsumerArg;

    void *cas_consumer(void *arg) {
        CASConsumerArg *c = (CASConsumerArg *)arg;
        sqlite3 *cdb = db_open(c->db_path);
        assert(cdb);
        c->consumed = 0;
        char holder[32];
        snprintf(holder, sizeof(holder), "cas-%d", c->id);

        for (int attempt = 0; attempt < 100; attempt++) {
            if (session_try_acquire(cdb, c->session_id, holder) == 0) {
                pthread_mutex_lock(&counter_lock);
                active_holders++;
                if (active_holders > max_concurrent)
                    max_concurrent = active_holders;
                pthread_mutex_unlock(&counter_lock);

                int n = inbox_consume_into_entries(cdb, c->session_id, 5);
                if (n > 0) c->consumed += n;

                pthread_mutex_lock(&counter_lock);
                active_holders--;
                pthread_mutex_unlock(&counter_lock);

                session_release(cdb, c->session_id, holder);
            }
            struct timespec ts = {0, 100000}; /* 0.1ms */
            nanosleep(&ts, NULL);
        }
        db_close(cdb);
        return NULL;
    }

    #define N_CAS 6
    pthread_t threads[N_CAS];
    CASConsumerArg args[N_CAS];
    for (int i = 0; i < N_CAS; i++) {
        args[i] = (CASConsumerArg){.db_path = DB_PATH, .session_id = sid, .id = i};
        pthread_create(&threads[i], NULL, cas_consumer, &args[i]);
    }
    for (int i = 0; i < N_CAS; i++)
        pthread_join(threads[i], NULL);

    /* CAS guarantee: max_concurrent must be exactly 1 */
    assert(max_concurrent == 1);

    /* All 100 items consumed */
    int total = 0;
    for (int i = 0; i < N_CAS; i++)
        total += args[i].consumed;
    assert(total == 100);
    #undef N_CAS

    printf("  PASS test_cas_no_overlap_under_load\n");
}

int main(void) {
    unlink(DB_PATH);
    printf("test_parallel_throughput:\n");
    test_parallel_produce_consume();
    test_interleaved_produce_consume();
    test_cas_no_overlap_under_load();
    unlink(DB_PATH);
    printf("All parallel throughput tests passed.\n");
    return 0;
}
