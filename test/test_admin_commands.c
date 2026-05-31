/* T145: admin command tests
 * V52: key write bypasses DB entirely
 * V53: non-admin chat_id rejected
 * Config reload picks up changes */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "telegram.h"
#include "config.h"
#include "db.h"

/* V52: telegram_write_env_key writes to file, never touches DB */
static void test_key_bypasses_db(void) {
    const char *env_path = "/tmp/cclaw_test_admin_env";
    const char *db_path = "/tmp/cclaw_test_admin.db";
    unlink(env_path);
    unlink(db_path);

    sqlite3 *db = db_open(db_path);
    assert(db);

    /* Write key to env file */
    assert(telegram_write_env_key(env_path, "OPENROUTER_API_KEY", "sk-secret-123") == 0);

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

    sql = "SELECT COUNT(*) FROM inbox WHERE payload LIKE '%sk-secret-123%';";
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    unlink(env_path);
    unlink(db_path);
}

/* V53: non-admin chat_id is rejected */
static void test_non_admin_rejected(void) {
    int64_t admin_ids[] = {111, 222};
    Config cfg = {0};
    cfg.admin_chat_ids = admin_ids;
    cfg.admin_chat_id_count = 2;

    /* Admin IDs pass */
    assert(telegram_is_admin(&cfg, 111) == 1);
    assert(telegram_is_admin(&cfg, 222) == 1);

    /* Non-admin IDs rejected */
    assert(telegram_is_admin(&cfg, 333) == 0);
    assert(telegram_is_admin(&cfg, 0) == 0);
    assert(telegram_is_admin(&cfg, -1) == 0);

    /* Empty config rejects all */
    Config empty = {0};
    assert(telegram_is_admin(&empty, 111) == 0);

    /* NULL config rejects */
    assert(telegram_is_admin(NULL, 111) == 0);
}

/* Config reload picks up changes from kv table */
static void test_config_reload(void) {
    const char *db_path = "/tmp/cclaw_test_admin_reload.db";
    unlink(db_path);

    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_PROVIDER");

    sqlite3 *db = db_open(db_path);
    assert(db);

    /* Set initial model */
    db_kv_set(db, "provider.model", "old-model");

    /* Load initial config */
    Config *cfg = config_load(db);
    assert(cfg);
    assert(strcmp(cfg->provider.model, "old-model") == 0);
    config_free(cfg);

    /* Update model via kv (simulates admin command path) */
    db_kv_set(db, "provider.model", "new-model-v2");

    /* Reload config — picks up change */
    cfg = config_load(db);
    assert(cfg);
    assert(strcmp(cfg->provider.model, "new-model-v2") == 0);
    config_free(cfg);

    /* Update endpoint */
    db_kv_set(db, "provider.base_url", "https://new-endpoint.ai/v1");
    cfg = config_load(db);
    assert(cfg);
    assert(strcmp(cfg->provider.base_url, "https://new-endpoint.ai/v1") == 0);
    config_free(cfg);

    unsetenv("OPENROUTER_API_KEY");
    db_close(db);
    unlink(db_path);
}

int main(void) {
    printf("test_key_bypasses_db...");
    test_key_bypasses_db();
    printf(" OK\n");

    printf("test_non_admin_rejected...");
    test_non_admin_rejected();
    printf(" OK\n");

    printf("test_config_reload...");
    test_config_reload();
    printf(" OK\n");

    printf("all admin command tests passed\n");
    return 0;
}
