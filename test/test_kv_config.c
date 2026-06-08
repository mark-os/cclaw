/* Test: config table seeding and provider access */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_kv_config.db"

static void test_fresh_db_defaults(void) {
    unlink(DB_PATH);
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    db_seed_defaults(db);

    /* Config table should have defaults */
    char *v = db_kv_get(db, "default_model");
    assert(v && strstr(v, "deepseek"));
    free(v);

    v = db_kv_get(db, "default_provider");
    assert(v && strcmp(v, "openrouter") == 0);
    free(v);

    /* Providers table should have openrouter */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT base_url FROM providers WHERE name='openrouter'",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "https://openrouter.ai/api/v1") == 0);
    sqlite3_finalize(s);

    db_close(db);
    unsetenv("OPENROUTER_API_KEY");
    unlink(DB_PATH);
    printf("  fresh_db_defaults... PASS\n");
}

int main(void) {
    printf("test_kv_config:\n");
    test_fresh_db_defaults();
    printf("all kv_config tests passed\n");
    return 0;
}
