/* The CacheExpiring timer (src/cache_expiring.c), the detector alone: the
 * clock is an argument and the arm is a row, so every case is a moment
 * chosen by the test — nothing sleeps. Proven in both directions: it fires
 * at 90% of the lifetime and not a second before; not on a busy session or
 * one with mail waiting; once per idle period; not for a cache that has
 * already lapsed (a daemon starting after a long stop); and again after the
 * next request. Firing the hooks is settle.c's (test_turn_settle). */
#define _POSIX_C_SOURCE 200809L
#include "cache_expiring.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DB_PATH = "/tmp/test_cclaw_cache_expiring.sqlite";
static sqlite3 *g_db;

static void run(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(g_db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "FAIL sql: %s\n  %s\n", err ? err : "?", sql);
        abort();
    }
}

static int64_t armed(int64_t sid) {
    return db_scalar_i64(g_db, "SELECT COALESCE(cache_armed_at, 0) FROM sessions WHERE id=?",
                         sid, -1);
}

static int take(time_t now, CacheDue *out) {
    CacheDue tmp[8];
    int n = cache_expiring_take(g_db, now, out ? out : tmp, 8);
    assert(n >= 0);
    return n;
}

static void set_state(int64_t sid, const char *state) {
    char sql[160];
    snprintf(sql, sizeof(sql), "UPDATE sessions SET state='%s' WHERE id=%lld;", state,
             (long long)sid);
    run(sql);
}

static void setup(void) {
    test_db_clean(DB_PATH);
    g_db = test_db_open(DB_PATH);
    assert(g_db);
    test_seed_agent(g_db, "Hour");      /* Anthropic: 3600 by default */
    test_seed_agent(g_db, "Five");      /* Anthropic, set to 300 */
    test_seed_agent(g_db, "Plain");     /* OpenAI-compatible, no lifetime */
    run("INSERT INTO providers(name, base_url, endpoint_type) VALUES"
        " ('a','http://a','anthropic'), ('o','http://o','openai');"
        "INSERT INTO models(id, provider_name, model, context_window) VALUES"
        " ('c@a','a','c',200000), ('m@o','o','m',128000);"
        "DELETE FROM agent_models;"
        "INSERT INTO agent_models(agent_name, model_id, pos) VALUES"
        " ('Hour','c@a',0), ('Plain','m@o',0);"
        "INSERT INTO agent_models(agent_name, model_id, pos, cache_lifetime) VALUES"
        " ('Five','c@a',0,300);");
}

static void test_arm(void) {
    int64_t h = session_create(g_db, "h", "Hour", -1, 0);
    int64_t p = session_create(g_db, "p", "Plain", -1, 0);
    assert(cache_expiring_arm(g_db, h, "Hour", 1000) == 0);
    assert(armed(h) == 1000);
    /* No lifetime on the governing entry: not armed, and an older arm goes. */
    run("UPDATE sessions SET cache_armed_at=5 WHERE agent_name='Plain';");
    assert(cache_expiring_arm(g_db, p, "Plain", 1000) == 0);
    assert(armed(p) == 0);
    /* A fallback lower in the list does not govern. */
    run("INSERT INTO agent_models(agent_name, model_id, pos) VALUES ('Plain','c@a',1);");
    assert(cache_expiring_arm(g_db, p, "Plain", 1000) == 0);
    assert(armed(p) == 0);
    printf("  PASS test_arm\n");
}

static void test_due_window(void) {
    int64_t s = session_create(g_db, "w", "Hour", -1, 0);
    const time_t T = 100000;
    assert(cache_expiring_arm(g_db, s, "Hour", T) == 0);
    /* Not before 90%. */
    assert(take(T, NULL) == 0);
    assert(take(T + 3239, NULL) == 0);
    assert(armed(s) == T);
    assert(cache_expiring_next(g_db, T + 10) == T + 3240);
    /* At 90%: claimed, with what the event needs. */
    CacheDue d[8];
    assert(take(T + 3240, d) == 1);
    assert(d[0].session_id == s && strcmp(d[0].agent, "Hour") == 0);
    assert(d[0].lifetime == 3600 && d[0].armed_at == T && d[0].depth == 0);
    /* Once per idle period. */
    assert(armed(s) == 0);
    assert(take(T + 3300, NULL) == 0);
    assert(cache_expiring_next(g_db, T + 3300) == 0);
    /* The next request arms it again, from its own start. */
    assert(cache_expiring_arm(g_db, s, "Hour", T + 5000) == 0);
    assert(take(T + 5000 + 3239, NULL) == 0);
    assert(take(T + 5000 + 3240, NULL) == 1);
    printf("  PASS test_due_window\n");
}

/* A 300-second entry is due at 270 — the window to lapse is 30 seconds. */
static void test_five_minutes(void) {
    int64_t s = session_create(g_db, "f", "Five", -1, 0);
    const time_t T = 200000;
    assert(cache_expiring_arm(g_db, s, "Five", T) == 0);
    assert(cache_expiring_next(g_db, T) == T + 270);
    assert(take(T + 269, NULL) == 0);
    CacheDue d[8];
    assert(take(T + 299, d) == 1 && d[0].lifetime == 300);
    printf("  PASS test_five_minutes\n");
}

/* Busy, or with a message waiting: the session is about to send a request
 * anyway. It stays armed and is taken once it is idle, inside the window. */
static void test_not_idle(void) {
    int64_t s = session_create(g_db, "b", "Hour", -1, 0);
    const time_t T = 300000;
    assert(cache_expiring_arm(g_db, s, "Hour", T) == 0);
    set_state(s, "llm_running");
    assert(take(T + 3300, NULL) == 0);
    assert(armed(s) == T);
    assert(cache_expiring_next(g_db, T) == 0);   /* the loop does not wait on it */
    set_state(s, "settling");
    assert(take(T + 3300, NULL) == 0);
    set_state(s, "idle");
    assert(inbox_insert(g_db, s, "cli", NULL, "hello") > 0);
    assert(take(T + 3300, NULL) == 0);
    assert(armed(s) == T);
    run("UPDATE inbox SET consumed=1;");
    assert(take(T + 3300, NULL) == 1);
    printf("  PASS test_not_idle\n");
}

/* At or past the lifetime the cache is gone: nothing to save, the arm goes,
 * nothing fires — however long ago (a daemon started after a long stop). A
 * busy session is disarmed the same way. */
static void test_lapsed(void) {
    int64_t a = session_create(g_db, "l1", "Hour", -1, 0);
    int64_t b = session_create(g_db, "l2", "Hour", -1, 0);
    int64_t c = session_create(g_db, "l3", "Hour", -1, 0);
    const time_t T = 400000;
    assert(cache_expiring_arm(g_db, a, "Hour", T) == 0);
    assert(cache_expiring_arm(g_db, b, "Hour", T - 86400 * 30) == 0);
    assert(cache_expiring_arm(g_db, c, "Hour", T) == 0);
    set_state(c, "llm_running");
    assert(take(T + 3600, NULL) == 0);
    assert(armed(a) == 0 && armed(b) == 0 && armed(c) == 0);
    /* And a governing entry whose lifetime was cleared since. */
    int64_t d = session_create(g_db, "l4", "Hour", -1, 0);
    assert(cache_expiring_arm(g_db, d, "Hour", T) == 0);
    run("UPDATE agent_models SET cache_lifetime=NULL WHERE agent_name='Hour';");
    assert(take(T + 10, NULL) == 0);
    assert(armed(d) == 0);
    run("UPDATE agent_models SET cache_lifetime=3600 WHERE agent_name='Hour';");
    printf("  PASS test_lapsed\n");
}

/* Any depth is taken: the depth is in the payload, the policy is the
 * script's (decision 24). */
static void test_depth(void) {
    int64_t parent = session_create(g_db, "dp", "Hour", -1, 0);
    int64_t child = session_create(g_db, "dc", "Hour", parent, 1);
    const time_t T = 500000;
    assert(cache_expiring_arm(g_db, child, "Hour", T) == 0);
    CacheDue d[8];
    assert(take(T + 3240, d) == 1 && d[0].session_id == child && d[0].depth == 1);
    printf("  PASS test_depth\n");
}

/* The payload's own numbers: depth, lifetime, idle time since the last
 * answer, that answer's cache counts (null when not reported). */
static void test_context(void) {
    int64_t s = session_create(g_db, "ctx", "Hour", -1, 0);
    ContextLimits lim = { .window = 200000, .threshold = 100000, .target = 50000,
                          .lifetime = 3600 };
    char *c = cache_expiring_context(g_db, s, 7000, 60000, &lim);
    assert(c);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(g_db,
        "SELECT json_extract(?1,'$.tokens') || ',' || json_extract(?1,'$.window') || ','"
        " || json_extract(?1,'$.trim_threshold') || ',' || json_extract(?1,'$.prune_target')"
        " || ',' || json_extract(?1,'$.depth') || ',' || json_extract(?1,'$.cache_lifetime')"
        " || ',' || json_type(?1,'$.idle_seconds') || ',' || json_type(?1,'$.cache_read_tokens')"
        " || ',' || json_type(?1,'$.cache_write_tokens')", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, c, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(st, 0),
                  "60000,200000,100000,50000,0,3600,null,null,null") == 0);
    sqlite3_finalize(st);
    free(c);

    char sql[320];
    snprintf(sql, sizeof(sql),
             "INSERT INTO llm_responses(session_id, iteration_id, model, status,"
             " cached_tokens, cache_write_tokens, created_at)"
             " VALUES (%lld, 1, 'c@a', 'ok', 1800, 248, 4000),"
             "        (%lld, 2, 'c@a', 'http_500', NULL, NULL, 4500);",
             (long long)s, (long long)s);
    run(sql);
    c = cache_expiring_context(g_db, s, 7000, 60000, &lim);
    assert(sqlite3_prepare_v2(g_db,
        "SELECT json_extract(?1,'$.idle_seconds') || ',' || json_extract(?1,'$.cache_read_tokens')"
        " || ',' || json_extract(?1,'$.cache_write_tokens')", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, c, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(st, 0), "3000,1800,248") == 0);
    sqlite3_finalize(st);
    free(c);
    printf("  PASS test_context\n");
}

int main(void) {
    TEST_INIT();
    printf("test_cache_expiring:\n");
    setup();
    test_arm();
    test_due_window();
    test_five_minutes();
    test_not_idle();
    test_lapsed();
    test_depth();
    test_context();
    db_close(g_db);
    test_db_clean(DB_PATH);
    printf("ALL PASSED\n");
    return 0;
}
