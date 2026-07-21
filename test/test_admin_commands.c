/* admin command tests (migrated to admin_api)
 * admin_set_key stores encrypted in secrets table, never plaintext
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
#include "test_util.h"

static const uint8_t TEST_KEY[32] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32
};

/* admin_set_key stores the key encrypted in secrets table — never plaintext in the DB */
static void test_key_encrypted_in_kv(void) {
    const char *db_path = "/tmp/cclaw_test_admin.db";
    test_db_clean(db_path);

    sqlite3 *db = test_db_open_seeded(db_path);
    assert(db);
    db_set_secret_key(TEST_KEY);

    /* Known provider maps to its canonical env-var name */
    assert(admin_set_key(db, "openrouter", "sk-secret-123") == 0);
    char *val = db_secret_get_system(db, "OPENROUTER_API_KEY");
    assert(val && strcmp(val, "sk-secret-123") == 0);
    free(val);

    /* Raw value in secrets table is ciphertext, not the plaintext key */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT value FROM secrets WHERE name='OPENROUTER_API_KEY'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *raw = (const char *)sqlite3_column_text(s, 0);
    assert(raw && strncmp(raw, "enc:", 4) == 0);
    assert(strstr(raw, "sk-secret-123") == NULL);
    sqlite3_finalize(s);

    /* Custom provider: VAR_NAME=value form */
    assert(admin_set_key(db, "custom", "MY_API_KEY=sk-custom-456") == 0);
    val = db_secret_get_system(db, "MY_API_KEY");
    assert(val && strcmp(val, "sk-custom-456") == 0);
    free(val);

    /* Malformed custom value rejected */
    assert(admin_set_key(db, "custom", "no-equals-sign") == -1);
    assert(admin_set_key(db, "unknown-provider", "sk-x") == -1);

    db_close(db);
    test_db_clean(db_path);
}

/* Admin operations update providers table correctly */
static void test_config_reload(void) {
    const char *db_path = "/tmp/cclaw_test_admin_reload.db";
    test_db_clean(db_path);

    sqlite3 *db = test_db_open_seeded(db_path);
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
    test_db_clean(db_path);
}

int main(void) {
    TEST_INIT();
    printf("test_key_encrypted_in_kv...");
    test_key_encrypted_in_kv();
    printf(" OK\n");

    printf("test_config_reload...");
    test_config_reload();
    printf(" OK\n");

    printf("all admin command tests passed\n");
    return 0;
}
