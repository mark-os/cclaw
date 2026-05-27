/* T188: test — daemon secret injection via env vars.
 * Verifies config_load_from_kv prefers CCLAW_INJECTED_* env vars over DB.
 * Cites V67. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "db.h"
#include "config.h"
#include "secret.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static char tmpdir[64];
static char db_path[128];

static void setup(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cclaw_t188_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
    snprintf(db_path, sizeof(db_path), "%s/test.db", tmpdir);
}

static void teardown(void) {
    unlink(db_path);
    char wal[160], shm[160];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    snprintf(shm, sizeof(shm), "%s-shm", db_path);
    unlink(wal);
    unlink(shm);
    char key_path[160];
    snprintf(key_path, sizeof(key_path), "%s/.cclaw_key", tmpdir);
    unlink(key_path);
    rmdir(tmpdir);
}

/* V67: injected env var overrides DB value for api_key */
static void test_injected_api_key(void) {
    TEST(injected_api_key);

    /* Clear any existing env */
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_INJECTED_API_KEY");

    sqlite3 *db = db_open(db_path);
    if (!db) FAIL("db_open");

    /* DB has a plaintext api_key from seeding */
    db_kv_set(db, "provider.api_key", "db-key-value");

    /* Set injected env var (simulating daemon injection) */
    setenv("CCLAW_INJECTED_API_KEY", "daemon-injected-key", 1);

    Config *cfg = config_load_from_kv(db);
    if (!cfg) { db_close(db); FAIL("config_load_from_kv"); }

    if (!cfg->provider.api_key || strcmp(cfg->provider.api_key, "daemon-injected-key") != 0) {
        config_free(cfg);
        db_close(db);
        FAIL("expected daemon-injected-key");
    }

    config_free(cfg);
    db_close(db);
    unsetenv("CCLAW_INJECTED_API_KEY");
    PASS();
}

/* V67: injected env var overrides DB value for telegram_token */
static void test_injected_telegram_token(void) {
    TEST(injected_telegram_token);

    unsetenv("CCLAW_TELEGRAM_TOKEN");
    unsetenv("CCLAW_INJECTED_TELEGRAM_TOKEN");

    sqlite3 *db = db_open(db_path);
    if (!db) FAIL("db_open");

    db_kv_set(db, "telegram_token", "db-token");

    setenv("CCLAW_INJECTED_TELEGRAM_TOKEN", "daemon-injected-token", 1);

    Config *cfg = config_load_from_kv(db);
    if (!cfg) { db_close(db); FAIL("config_load_from_kv"); }

    if (!cfg->telegram_token || strcmp(cfg->telegram_token, "daemon-injected-token") != 0) {
        config_free(cfg);
        db_close(db);
        FAIL("expected daemon-injected-token");
    }

    config_free(cfg);
    db_close(db);
    unsetenv("CCLAW_INJECTED_TELEGRAM_TOKEN");
    PASS();
}

/* V67: without injected env, falls back to DB value */
static void test_fallback_to_db(void) {
    TEST(fallback_to_db);

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_INJECTED_API_KEY");
    unsetenv("CCLAW_INJECTED_TELEGRAM_TOKEN");
    unsetenv("CCLAW_TELEGRAM_TOKEN");

    sqlite3 *db = db_open(db_path);
    if (!db) FAIL("db_open");

    db_kv_set(db, "provider.api_key", "plain-db-key");
    db_kv_set(db, "telegram_token", "plain-db-token");

    Config *cfg = config_load_from_kv(db);
    if (!cfg) { db_close(db); FAIL("config_load_from_kv"); }

    if (!cfg->provider.api_key || strcmp(cfg->provider.api_key, "plain-db-key") != 0) {
        config_free(cfg);
        db_close(db);
        FAIL("expected plain-db-key from DB");
    }
    if (!cfg->telegram_token || strcmp(cfg->telegram_token, "plain-db-token") != 0) {
        config_free(cfg);
        db_close(db);
        FAIL("expected plain-db-token from DB");
    }

    config_free(cfg);
    db_close(db);
    PASS();
}

int main(void) {
    printf("test_daemon_secrets (T188):\n");
    setup();
    test_injected_api_key();
    test_injected_telegram_token();
    test_fallback_to_db();
    teardown();
    printf("  %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
