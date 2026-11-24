#include "db.h"
#include "config_registry.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *DB_PATH = "/tmp/test_cclaw_models.db";

/* The seeded default model row, and the health defaults the registry supplies. */
static void test_models_table(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    db_seed_defaults(db);

    /* context_window NULL = use the global default */
    assert(test_qi(db, "SELECT COUNT(*) FROM models"
                       " WHERE id='openrouter/deepseek/deepseek-v4.1-flash'"
                       " AND provider_name='openrouter' AND model='deepseek/deepseek-v4.1-flash'"
                       " AND context_window IS NULL AND status='healthy'") == 1);
    assert(config_get_int(db, "health_fail_threshold") == 4);
    assert(config_get_int(db, "health_cooldown_sec") == 300);
    assert(config_get_int(db, "notify_model_change") == 1);

    db_close(db);
    test_db_clean(DB_PATH);
}

int main(void) {
    TEST_INIT();
    printf("test_models_jobs:\n");
    RUN(test_models_table);
    return 0;
}
