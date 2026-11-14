#include "db.h"
#include "config_registry.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_models.db";

static void test_models_table(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    db_seed_defaults(db);

    /* Verify models row seeded */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT provider_name, model, context_window, status"
        " FROM models WHERE id='openrouter/deepseek/deepseek-v4-flash'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "openrouter") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "deepseek/deepseek-v4-flash") == 0);
    assert(sqlite3_column_type(s, 2) == SQLITE_NULL);  /* NULL = use global default */
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "healthy") == 0);
    sqlite3_finalize(s);

    /* Verify health config defaults via registry */
    assert(config_get_int(db, "health_fail_threshold") == 4);
    assert(config_get_int(db, "health_cooldown_sec") == 300);
    assert(config_get_int(db, "notify_model_change") == 1);

    db_close(db); test_db_clean(DB_PATH);
    printf("  PASS test_models_table\n");
}

int main(void) {
    TEST_INIT();
    printf("test_models_jobs:\n");
    test_models_table();
    printf("All models/jobs tests passed.\n");
    return 0;
}
