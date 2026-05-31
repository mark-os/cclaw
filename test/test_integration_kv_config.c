/* T175: integration test — kv config lifecycle.
 * Empty DB → defaults seeded; env var overrides win;
 * config_load produces correct Config struct; no network.
 * Cites V61, T169. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "db.h"
#include "config.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static const char *DB_PATH = "/tmp/test_integration_kv_config.db";

static void cleanup(void) {
    unlink(DB_PATH);
    char wal[256], shm[256];
    snprintf(wal, sizeof(wal), "%s-wal", DB_PATH);
    snprintf(shm, sizeof(shm), "%s-shm", DB_PATH);
    unlink(wal);
    unlink(shm);
}

/* Fresh file DB → defaults seeded → config correct */
static void test_fresh_db_defaults(void) {
    TEST(fresh_db_defaults);
    cleanup();

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");

    /* Verify kv defaults exist */
    char *v = db_kv_get(db, "provider.base_url");
    if (!v || strcmp(v, "https://openrouter.ai/api/v1") != 0) { free(v); db_close(db); FAIL("base_url default"); }
    free(v);

    v = db_kv_get(db, "provider.model");
    if (!v || strcmp(v, "deepseek/deepseek-v4-flash") != 0) { free(v); db_close(db); FAIL("model default"); }
    free(v);

    /* Load config struct */
    Config *cfg = config_load(db);
    if (!cfg) { db_close(db); FAIL("config_load"); }

    assert(strcmp(cfg->provider.base_url, "https://openrouter.ai/api/v1") == 0);
    assert(strcmp(cfg->provider.model, "deepseek/deepseek-v4-flash") == 0);
    assert(cfg->provider.max_tokens == 4096);
    assert(cfg->provider.context_window == 65536);
    assert(cfg->web_port == 8080);
    assert(cfg->max_iterations == 25);
    assert(cfg->shell_timeout == 30);
    assert(strcmp(cfg->workspace, "./workspace") == 0);

    config_free(cfg);
    db_close(db);
    PASS();
}

/* Modify kv → close → reopen → values persist */
static void test_persistence_across_reopen(void) {
    TEST(persistence_across_reopen);
    cleanup();

    /* First open: seed + modify */
    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open 1");

    db_kv_set(db, "provider.model", "custom/model-v2");
    db_kv_set(db, "max_iterations", "42");
    db_close(db);

    /* Second open: verify persisted */
    db = db_open(DB_PATH);
    if (!db) FAIL("db_open 2");

    Config *cfg = config_load(db);
    if (!cfg) { db_close(db); FAIL("config_load"); }

    assert(strcmp(cfg->provider.model, "custom/model-v2") == 0);
    assert(cfg->max_iterations == 42);
    /* Unmodified defaults still correct */
    assert(strcmp(cfg->provider.base_url, "https://openrouter.ai/api/v1") == 0);
    assert(cfg->web_port == 8080);

    config_free(cfg);
    db_close(db);
    PASS();
}

/* Env vars override kv values (V61 priority: env > kv > default) */
static void test_env_overrides_kv(void) {
    TEST(env_overrides_kv);
    cleanup();

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");

    /* Set kv values */
    db_kv_set(db, "provider.model", "kv-model");
    db_kv_set(db, "max_iterations", "10");
    db_kv_set(db, "web_port", "3000");

    /* Set env overrides */
    setenv("CCLAW_MODEL", "env-model", 1);
    setenv("CCLAW_MAX_ITERATIONS", "77", 1);

    Config *cfg = config_load(db);
    if (!cfg) { db_close(db); FAIL("config_load"); }

    /* Env wins */
    assert(strcmp(cfg->provider.model, "env-model") == 0);
    assert(cfg->max_iterations == 77);
    /* Non-overridden kv value preserved */
    assert(cfg->web_port == 3000);

    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_MAX_ITERATIONS");
    config_free(cfg);
    db_close(db);
    PASS();
}

/* OPENROUTER_API_KEY env → seeded into kv on fresh DB */
static void test_api_key_env_seed(void) {
    TEST(api_key_env_seed);
    cleanup();

    setenv("OPENROUTER_API_KEY", "sk-or-v1-test123", 1);

    sqlite3 *db = db_open(DB_PATH);
    if (!db) { unsetenv("OPENROUTER_API_KEY"); FAIL("db_open"); }

    Config *cfg = config_load(db);
    if (!cfg) { db_close(db); unsetenv("OPENROUTER_API_KEY"); FAIL("config_load"); }

    assert(cfg->provider.api_key && strcmp(cfg->provider.api_key, "sk-or-v1-test123") == 0);

    config_free(cfg);
    db_close(db);
    unsetenv("OPENROUTER_API_KEY");
    PASS();
}

/* No API key env → config.api_key is NULL */
static void test_no_api_key(void) {
    TEST(no_api_key);
    cleanup();

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("GEMINI_API_KEY");

    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open");

    Config *cfg = config_load(db);
    if (!cfg) { db_close(db); FAIL("config_load"); }

    assert(cfg->provider.api_key == NULL);

    config_free(cfg);
    db_close(db);
    PASS();
}

/* Defaults not overwritten on second open of existing DB */
static void test_no_reseed_existing_db(void) {
    TEST(no_reseed_existing_db);
    cleanup();

    /* First open: seeds defaults, then modify */
    sqlite3 *db = db_open(DB_PATH);
    if (!db) FAIL("db_open 1");
    db_kv_set(db, "provider.model", "modified-model");
    db_close(db);

    /* Second open: should NOT overwrite modified value with default */
    db = db_open(DB_PATH);
    if (!db) FAIL("db_open 2");

    char *v = db_kv_get(db, "provider.model");
    if (!v || strcmp(v, "modified-model") != 0) { free(v); db_close(db); FAIL("value overwritten"); }
    free(v);

    db_close(db);
    PASS();
}

int main(void) {
    printf("test_integration_kv_config:\n");
    test_fresh_db_defaults();
    test_persistence_across_reopen();
    test_env_overrides_kv();
    test_api_key_env_seed();
    test_no_api_key();
    test_no_reseed_existing_db();
    cleanup();
    printf("  %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
