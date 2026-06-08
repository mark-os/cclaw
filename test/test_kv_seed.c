#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void test_config_seed(void) {
    unlink("/tmp/test_config_seed.db");
    sqlite3 *db = db_open("/tmp/test_config_seed.db");
    assert(db);

    /* Seed should populate config + providers */
    db_seed_defaults(db);

    /* Fresh DB should have config defaults */
    char *v = db_kv_get(db, "default_model");
    assert(v && strstr(v, "deepseek"));
    free(v);

    v = db_kv_get(db, "default_provider");
    assert(v && strcmp(v, "openrouter") == 0);
    free(v);

    /* Should have seeded providers table */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM providers", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) >= 1);
    sqlite3_finalize(s);

    db_close(db);
    unlink("/tmp/test_config_seed.db");
}

int main(void) {
    printf("test_config_seed...");
    test_config_seed();
    printf(" OK\n");
    return 0;
}
