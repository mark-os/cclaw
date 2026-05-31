#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TEST_DB = "/tmp/cclaw_test_config.sqlite";

static sqlite3 *fresh_db(void) {
    unlink(TEST_DB);
    return db_open(TEST_DB);
}

static void test_defaults(void) {
    /* No kv overrides, just env */
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_TELEGRAM_TOKEN");
    unsetenv("CCLAW_DB_PATH");
    unsetenv("CCLAW_WEB_PORT");
    setenv("OPENROUTER_API_KEY", "sk-test-123", 1);

    sqlite3 *db = fresh_db();
    assert(db);
    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.api_key, "sk-test-123") == 0);
    assert(strcmp(cfg->provider.base_url, "https://openrouter.ai/api/v1") == 0);
    assert(strcmp(cfg->provider.model, "deepseek/deepseek-v4-flash") == 0);
    assert(cfg->web_port == 8080);
    assert(cfg->max_iterations == 25);
    config_free(cfg);
    db_close(db);
    unsetenv("OPENROUTER_API_KEY");
    printf("  PASS: test_defaults\n");
}

static void test_kv_values(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_DB_PATH");
    unsetenv("CCLAW_WEB_PORT");

    sqlite3 *db = fresh_db();
    assert(db);
    db_kv_set(db, "provider.base_url", "http://localhost:8000/v1");
    db_kv_set(db, "provider.model", "gpt-4");
    db_kv_set(db, "provider.max_tokens", "2048");
    db_kv_set(db, "web_port", "9090");

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.base_url, "http://localhost:8000/v1") == 0);
    assert(strcmp(cfg->provider.model, "gpt-4") == 0);
    assert(cfg->provider.max_tokens == 2048);
    assert(cfg->web_port == 9090);
    config_free(cfg);
    db_close(db);
    printf("  PASS: test_kv_values\n");
}

static void test_env_overrides_kv(void) {
    sqlite3 *db = fresh_db();
    assert(db);
    db_kv_set(db, "provider.model", "gpt-4");

    setenv("OPENROUTER_API_KEY", "env-key", 1);
    setenv("CCLAW_MODEL", "claude-4", 1);

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    /* Env overrides kv */
    assert(strcmp(cfg->provider.api_key, "env-key") == 0);
    assert(strcmp(cfg->provider.model, "claude-4") == 0);
    config_free(cfg);

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_MODEL");
    db_close(db);
    printf("  PASS: test_env_overrides_kv\n");
}

static void test_fallback_providers(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");

    sqlite3 *db = fresh_db();
    assert(db);
    db_kv_set(db, "fallback_providers",
        "[{\"base_url\":\"https://gemini.example.com/v1\",\"api_key\":\"gem-key\","
        "\"model\":\"gemma-4-31b-it\",\"max_tokens\":1024},"
        "{\"base_url\":\"https://backup.example.com/v1\",\"api_key\":\"bak-key\","
        "\"model\":\"backup-model\"}]");

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->fallback_count == 2);
    assert(strcmp(cfg->fallback_providers[0].base_url, "https://gemini.example.com/v1") == 0);
    assert(strcmp(cfg->fallback_providers[0].api_key, "gem-key") == 0);
    assert(strcmp(cfg->fallback_providers[0].model, "gemma-4-31b-it") == 0);
    assert(cfg->fallback_providers[0].max_tokens == 1024);
    assert(strcmp(cfg->fallback_providers[1].base_url, "https://backup.example.com/v1") == 0);
    assert(strcmp(cfg->fallback_providers[1].api_key, "bak-key") == 0);
    assert(strcmp(cfg->fallback_providers[1].model, "backup-model") == 0);
    /* Inherits primary max_tokens as default */
    assert(cfg->fallback_providers[1].max_tokens == 4096);
    config_free(cfg);
    db_close(db);
    printf("  PASS: test_fallback_providers\n");
}

static void test_stale_lock_timeout(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_STALE_LOCK_TIMEOUT");

    sqlite3 *db = fresh_db();
    assert(db);
    db_kv_set(db, "stale_lock_timeout", "120");

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->stale_lock_timeout == 120);
    config_free(cfg);
    db_close(db);

    /* Env override */
    setenv("CCLAW_STALE_LOCK_TIMEOUT", "60", 1);
    db = fresh_db();
    Config *cfg2 = config_load(db);
    assert(cfg2 != NULL);
    assert(cfg2->stale_lock_timeout == 60);
    config_free(cfg2);
    db_close(db);

    unsetenv("CCLAW_STALE_LOCK_TIMEOUT");
    printf("  PASS: test_stale_lock_timeout\n");
}

static void test_system_prompt(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_SYSTEM_PROMPT");

    /* Test default (no system_prompt in kv) */
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    sqlite3 *db = fresh_db();
    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->system_prompt == NULL);
    char *rendered = config_render_system_prompt(cfg, 42);
    assert(rendered != NULL);
    assert(strstr(rendered, "CClaw") != NULL);
    assert(strstr(rendered, "Tool Call Style") != NULL);
    assert(strstr(rendered, "Execution Bias") != NULL);
    free(rendered);
    config_free(cfg);
    db_close(db);

    /* Test env override */
    setenv("CCLAW_SYSTEM_PROMPT", "env prompt {session_id}", 1);
    db = fresh_db();
    cfg = config_load(db);
    assert(cfg != NULL);
    rendered = config_render_system_prompt(cfg, 7);
    assert(strstr(rendered, "env prompt 7") != NULL);
    free(rendered);
    config_free(cfg);
    db_close(db);

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_SYSTEM_PROMPT");
    printf("  PASS: test_system_prompt\n");
}

static void test_admin_chat_ids(void) {
    unsetenv("OPENROUTER_API_KEY");

    sqlite3 *db = fresh_db();
    assert(db);
    db_kv_set(db, "admin_chat_ids", "[111222333, 444555666]");

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->admin_chat_id_count == 2);
    assert(cfg->admin_chat_ids[0] == 111222333);
    assert(cfg->admin_chat_ids[1] == 444555666);
    config_free(cfg);
    db_close(db);

    /* No admin_chat_ids → count 0, pointer NULL */
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    db = fresh_db();
    cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->admin_chat_id_count == 0);
    assert(cfg->admin_chat_ids == NULL);
    config_free(cfg);
    db_close(db);

    unsetenv("OPENROUTER_API_KEY");
    printf("  PASS: test_admin_chat_ids\n");
}

int main(void) {
    printf("test_config:\n");
    test_defaults();
    test_kv_values();
    test_env_overrides_kv();
    test_fallback_providers();
    test_stale_lock_timeout();
    test_system_prompt();
    test_admin_chat_ids();
    printf("All config tests passed.\n");
    return 0;
}
