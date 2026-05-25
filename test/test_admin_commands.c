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
    const char *sql = "SELECT COUNT(*) FROM entries WHERE data LIKE '%sk-secret-123%';";
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

/* Config reload picks up changes after config_update_model */
static void test_config_reload(void) {
    const char *path = "/tmp/cclaw_test_admin_cfg.json";
    const char *json =
        "{\n"
        "  \"provider\": {\"base_url\": \"https://openrouter.ai/api/v1\", \"model\": \"old-model\"},\n"
        "  \"providers\": [{\"base_url\": \"https://gemini.api\", \"model\": \"gemini-old\"}]\n"
        "}";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    setenv("OPENROUTER_API_KEY", "sk-test", 1);

    /* Load initial config */
    Config *cfg = config_load(path);
    assert(cfg);
    assert(strcmp(cfg->provider.model, "old-model") == 0);
    config_free(cfg);

    /* Update model via admin command path */
    assert(config_update_model(path, 0, "new-model-v2") == 0);

    /* Reload config — picks up change */
    cfg = config_load(path);
    assert(cfg);
    assert(strcmp(cfg->provider.model, "new-model-v2") == 0);
    /* Fallback unchanged */
    assert(cfg->fallback_count == 1);
    assert(strcmp(cfg->fallback_providers[0].model, "gemini-old") == 0);
    config_free(cfg);

    /* Update endpoint */
    assert(config_update_endpoint(path, 0, "https://new-endpoint.ai/v1") == 0);
    cfg = config_load(path);
    assert(cfg);
    assert(strcmp(cfg->provider.base_url, "https://new-endpoint.ai/v1") == 0);
    config_free(cfg);

    unsetenv("OPENROUTER_API_KEY");
    remove(path);
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
