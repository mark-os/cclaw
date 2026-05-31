#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "db.h"
#include "config.h"
#include "secret.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } while(0)

static void test_config_load_defaults(void) {
    /* Fresh DB with seeded defaults → config_load returns correct values */
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    Config *cfg = config_load(db);
    assert(cfg != NULL);

    assert(cfg->provider.base_url && strcmp(cfg->provider.base_url, "https://openrouter.ai/api/v1") == 0);
    assert(cfg->provider.model && strcmp(cfg->provider.model, "deepseek/deepseek-v4-flash") == 0);
    assert(cfg->provider.max_tokens == 4096);
    assert(cfg->provider.context_window == 65536);
    assert(cfg->provider.cache_hints == CACHE_HINTS_AUTO);
    assert(cfg->web_port == 8080);
    assert(cfg->max_iterations == 25);
    assert(cfg->max_history_tokens == 0);
    assert(cfg->heartbeat_interval == 0);
    assert(cfg->shell_timeout == 30);
    assert(cfg->workspace && strcmp(cfg->workspace, "./workspace") == 0);
    assert(cfg->fallback_count == 0);
    assert(cfg->admin_chat_id_count == 0);

    config_free(cfg);
    db_close(db);
    printf("  PASS: config_load defaults\n");
}

static void test_config_load_custom(void) {
    /* Modify kv values → config reflects changes */
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    db_kv_set(db, "provider.model", "anthropic/claude-3.5-sonnet");
    db_kv_set(db, "provider.max_tokens", "8192");
    db_kv_set(db, "web_port", "9090");
    db_kv_set(db, "max_iterations", "50");
    db_kv_set(db, "workspace", "/tmp/test-ws");

    Config *cfg = config_load(db);
    assert(cfg != NULL);

    assert(strcmp(cfg->provider.model, "anthropic/claude-3.5-sonnet") == 0);
    assert(cfg->provider.max_tokens == 8192);
    assert(cfg->web_port == 9090);
    assert(cfg->max_iterations == 50);
    assert(strcmp(cfg->workspace, "/tmp/test-ws") == 0);

    config_free(cfg);
    db_close(db);
    printf("  PASS: config_load custom values\n");
}

static void test_config_env_overrides_kv(void) {
    /* Env vars take priority over kv values */
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    db_kv_set(db, "provider.model", "kv-model");
    setenv("CCLAW_MODEL", "env-model", 1);
    setenv("CCLAW_MAX_ITERATIONS", "99", 1);

    Config *cfg = config_load(db);
    assert(cfg != NULL);

    assert(strcmp(cfg->provider.model, "env-model") == 0);
    assert(cfg->max_iterations == 99);

    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_MAX_ITERATIONS");
    config_free(cfg);
    db_close(db);
    printf("  PASS: env overrides kv\n");
}

static void test_config_api_key_from_kv(void) {
    /* api_key read via secret getter */
    unsetenv("OPENROUTER_API_KEY");
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    db_kv_set(db, "provider.api_key", "sk-test-secret");

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->provider.api_key && strcmp(cfg->provider.api_key, "sk-test-secret") == 0);

    config_free(cfg);
    db_close(db);
    printf("  PASS: api_key from kv\n");
}

static void test_config_fallback_providers(void) {
    /* JSON array in kv → parsed into fallback_providers */
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    db_kv_set(db, "fallback_providers",
        "[{\"base_url\":\"https://api.google.com/v1\",\"model\":\"gemini-pro\",\"max_tokens\":2048}]");

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->fallback_count == 1);
    assert(cfg->fallback_providers[0].base_url &&
           strcmp(cfg->fallback_providers[0].base_url, "https://api.google.com/v1") == 0);
    assert(cfg->fallback_providers[0].model &&
           strcmp(cfg->fallback_providers[0].model, "gemini-pro") == 0);
    assert(cfg->fallback_providers[0].max_tokens == 2048);

    config_free(cfg);
    db_close(db);
    printf("  PASS: fallback providers from kv\n");
}

static void test_config_admin_chat_ids(void) {
    /* JSON array of chat IDs */
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    db_kv_set(db, "admin_chat_ids", "[123456789, 987654321]");

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->admin_chat_id_count == 2);
    assert(cfg->admin_chat_ids[0] == 123456789);
    assert(cfg->admin_chat_ids[1] == 987654321);

    config_free(cfg);
    db_close(db);
    printf("  PASS: admin_chat_ids from kv\n");
}

static void test_kv_secret_passthrough(void) {
    /* Without secret key loaded, secret functions are plaintext passthrough */
    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    assert(db_kv_set_secret(db, "test.secret", "my-secret-value") == 0);
    char *v = db_kv_get_secret(db, "test.secret");
    assert(v && strcmp(v, "my-secret-value") == 0);
    free(v);

    db_close(db);
    printf("  PASS: kv secret passthrough (no key)\n");
}

static void test_kv_secret_encrypted(void) {
    /* With secret key loaded, values are encrypted */
    static const uint8_t key[32] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,
                                    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32};
    db_set_secret_key(key);

    sqlite3 *db = db_open(":memory:");
    if (!db) FAIL("db_open");

    assert(db_kv_set_secret(db, "test.secret", "encrypted-value") == 0);

    /* Raw value should be enc: prefixed */
    char *raw = db_kv_get(db, "test.secret");
    assert(raw && strncmp(raw, "enc:", 4) == 0);
    free(raw);

    /* Secret getter decrypts */
    char *v = db_kv_get_secret(db, "test.secret");
    assert(v && strcmp(v, "encrypted-value") == 0);
    free(v);

    db_close(db);
    printf("  PASS: kv secret encrypted\n");
}

int main(void) {
    printf("test_kv_config:\n");
    test_config_load_defaults();
    test_config_load_custom();
    test_config_env_overrides_kv();
    test_config_api_key_from_kv();
    test_config_fallback_providers();
    test_config_admin_chat_ids();
    test_kv_secret_passthrough();
    test_kv_secret_encrypted();
    printf("All kv_config tests passed.\n");
    return 0;
}
