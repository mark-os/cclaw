/* Test: config registry pipeline and provider access */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "config_registry.h"
#include "test_util.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_kv_config.db"

static void test_fresh_db_defaults(void) {
    test_db_clean(DB_PATH);
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    db_seed_defaults(db);
    config_registry_sync(db);

    /* Config table should have registry defaults after sync */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT default_value, description FROM config WHERE key='max_iterations'",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "25") == 0);
    /* description must be non-empty */
    const char *desc = (const char *)sqlite3_column_text(s, 1);
    assert(desc != NULL && strlen(desc) > 0);
    sqlite3_finalize(s);

    /* config_get returns override when value is set, default otherwise */
    assert(config_get_int(db, "max_iterations") == 25);
    config_set(db, "max_iterations", "50");
    assert(config_get_int(db, "max_iterations") == 50);
    config_set(db, "max_iterations", NULL);
    assert(config_get_int(db, "max_iterations") == 25);

    /* Providers table should have openrouter */
    assert(sqlite3_prepare_v2(db, "SELECT base_url FROM providers WHERE name='openrouter'",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "https://openrouter.ai/api/v1") == 0);
    sqlite3_finalize(s);

    db_close(db);
    unsetenv("OPENROUTER_API_KEY");
    test_db_clean(DB_PATH);
    printf("  fresh_db_defaults... PASS\n");
}

int main(void) {
    TEST_INIT();
    printf("test_kv_config:\n");
    test_fresh_db_defaults();
    printf("all kv_config tests passed\n");
    return 0;
}
