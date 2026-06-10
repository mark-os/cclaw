/* T145: admin command tests (migrated to admin_api)
 * V52: key write bypasses DB entirely
 * Config reload picks up changes */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "admin_api.h"
#include <sqlite3.h>
#include "db.h"

/* V52: admin_write_env_key writes to file, never touches DB */
static void test_key_bypasses_db(void) {
    const char *env_path = "/tmp/cclaw_test_admin_env";
    const char *db_path = "/tmp/cclaw_test_admin.db";
    unlink(env_path);
    unlink(db_path);

    sqlite3 *db = test_db_open(db_path);
    assert(db);

    /* Write key to env file */
    assert(admin_write_env_key(env_path, "OPENROUTER_API_KEY", "sk-secret-123") == 0);

    /* Verify key is in env file */
    FILE *f = fopen(env_path, "r");
    assert(f);
    char buf[256];
    assert(fgets(buf, sizeof(buf), f));
    assert(strstr(buf, "sk-secret-123") != NULL);
    fclose(f);

    /* Verify DB has no trace of the key value */
    sqlite3_stmt *stmt;
    const char *sql = "SELECT COUNT(*) FROM entries WHERE content LIKE '%sk-secret-123%';";
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    unlink(env_path);
    unlink(db_path);
}

/* Admin operations update providers table correctly */
static void test_config_reload(void) {
    const char *db_path = "/tmp/cclaw_test_admin_reload.db";
    unlink(db_path);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    db_seed_defaults(db);

    /* Update model via admin API */
    assert(admin_set_model(db, 0, "new-model-v2") == 0);

    /* Verify */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT default_model FROM providers WHERE priority=0", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "new-model-v2") == 0);
    sqlite3_finalize(s);

    /* Update endpoint */
    assert(admin_set_endpoint(db, 0, "https://new-endpoint.ai/v1") == 0);
    sqlite3_prepare_v2(db, "SELECT base_url FROM providers WHERE priority=0", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "https://new-endpoint.ai/v1") == 0);
    sqlite3_finalize(s);

    db_close(db);
    unlink(db_path);
}

int main(void) {
    printf("test_key_bypasses_db...");
    test_key_bypasses_db();
    printf(" OK\n");

    printf("test_config_reload...");
    test_config_reload();
    printf(" OK\n");

    printf("all admin command tests passed\n");
    return 0;
}
