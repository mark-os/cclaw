#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_models.db";

static void test_models_table(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    db_seed_defaults(db);

    /* Verify models row seeded */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT provider_name, model, context_window, status"
        " FROM models WHERE id='openrouter/deepseek/deepseek-v4-flash'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "openrouter") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "deepseek/deepseek-v4-flash") == 0);
    assert(sqlite3_column_int(s, 2) == 128000);
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "healthy") == 0);
    sqlite3_finalize(s);

    /* Verify health config defaults */
    char *v = db_kv_get(db, "health_5xx_threshold");
    assert(v && strcmp(v, "3") == 0); free(v);
    v = db_kv_get(db, "health_429_threshold");
    assert(v && strcmp(v, "10") == 0); free(v);
    v = db_kv_get(db, "health_cooldown_sec");
    assert(v && strcmp(v, "300") == 0); free(v);

    db_close(db); unlink(DB_PATH);
    printf("  PASS test_models_table\n");
}

static void test_llm_jobs_claim(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    /* Insert a job */
    sqlite3_stmt *ins;
    assert(sqlite3_prepare_v2(db, "INSERT INTO llm_jobs(session_id, agent_name, recall)"
        " VALUES(42, 'default', 1)", -1, &ins, NULL) == SQLITE_OK);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);

    /* Atomic claim via UPDATE ... RETURNING */
    sqlite3_stmt *claim;
    const char *sql = "UPDATE llm_jobs SET status='active', claimed_at=unixepoch()"
        " WHERE id = (SELECT id FROM llm_jobs WHERE status='pending' ORDER BY id LIMIT 1)"
        " RETURNING id, session_id, agent_name, recall";
    assert(sqlite3_prepare_v2(db, sql, -1, &claim, NULL) == SQLITE_OK);
    assert(sqlite3_step(claim) == SQLITE_ROW);
    int64_t job_id = sqlite3_column_int64(claim, 0);
    int64_t sid = sqlite3_column_int64(claim, 1);
    const char *agent = (const char *)sqlite3_column_text(claim, 2);
    int recall = sqlite3_column_int(claim, 3);
    assert(job_id == 1);
    assert(sid == 42);
    assert(strcmp(agent, "default") == 0);
    assert(recall == 1);
    sqlite3_finalize(claim);

    /* Second claim should return nothing (already claimed) */
    assert(sqlite3_prepare_v2(db, sql, -1, &claim, NULL) == SQLITE_OK);
    assert(sqlite3_step(claim) == SQLITE_DONE); /* no row */
    sqlite3_finalize(claim);

    db_close(db); unlink(DB_PATH);
    printf("  PASS test_llm_jobs_claim\n");
}

static void test_providers_status_column(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    db_seed_defaults(db);

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT status FROM providers WHERE name='openrouter'",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "healthy") == 0);
    sqlite3_finalize(s);

    db_close(db); unlink(DB_PATH);
    printf("  PASS test_providers_status_column\n");
}

int main(void) {
    printf("test_models_jobs:\n");
    test_models_table();
    test_llm_jobs_claim();
    test_providers_status_column();
    printf("All models/jobs tests passed.\n");
    return 0;
}
