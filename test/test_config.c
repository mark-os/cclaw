#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "config_registry.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *TEST_DB = "/tmp/cclaw_test_config.sqlite";

static sqlite3 *fresh_db(void) {
    unlink(TEST_DB);
    return test_db_open(TEST_DB);
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
    /* Remove auto-seeded provider, set our own */
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,endpoint_type,api_key_env,default_model,priority)"
        " VALUES('test','http://localhost:8000/v1','openai','OPENROUTER_API_KEY','gpt-4',0);", NULL, NULL, NULL);
    config_set(db, "web_port", "9090");

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.base_url, "http://localhost:8000/v1") == 0);
    assert(strcmp(cfg->provider.model, "gpt-4") == 0);
    assert(cfg->web_port == 9090);
    config_free(cfg);
    db_close(db);
    printf("  PASS: test_kv_values\n");
}

static void test_env_overrides_kv(void) {
    sqlite3 *db = fresh_db();
    assert(db);

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

static void test_kv_secret_fallback(void) {
    /* Third-priority key source: env unset → encrypted secret in secrets table */
    unsetenv("OPENROUTER_API_KEY");
    static const uint8_t k[32] = {9, 8, 7, 6, 5, 4, 3, 2, 1};

    sqlite3 *db = fresh_db();
    assert(db);
    db_set_secret_key(k);
    assert(db_secret_set(db, "OPENROUTER_API_KEY", "sk-from-kv", "operator", "system") == 0);

    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(cfg->provider.api_key != NULL);
    assert(strcmp(cfg->provider.api_key, "sk-from-kv") == 0);
    config_free(cfg);
    db_close(db);
    printf("  PASS: test_kv_secret_fallback\n");
}

static void test_fallback_providers(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    sqlite3_exec(db, "INSERT INTO providers(name,base_url,endpoint_type,api_key_env,default_model,priority)"
        " VALUES('primary','http://p.com/v1','openai','KEY1','model-a',0);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO providers(name,base_url,endpoint_type,api_key_env,default_model,priority)"
        " VALUES('fb1','http://fb1.com/v1','openai','KEY2','model-b',1);", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO providers(name,base_url,endpoint_type,api_key_env,default_model,priority)"
        " VALUES('fb2','http://fb2.com/v1','gemini','KEY3','model-c',2);", NULL, NULL, NULL);

    Config *cfg = config_load(db);
    assert(cfg);
    assert(strcmp(cfg->provider.base_url, "http://p.com/v1") == 0);
    assert(strcmp(cfg->provider.model, "model-a") == 0);
    assert(cfg->fallback_count == 2);
    assert(strcmp(cfg->fallback_providers[0].model, "model-b") == 0);
    assert(strcmp(cfg->fallback_providers[1].model, "model-c") == 0);
    assert(cfg->fallback_providers[1].endpoint_type == ENDPOINT_GEMINI);
    config_free(cfg);
    db_close(db);
    printf("  PASS: test_fallback_providers\n");
}

static void test_stale_lock_timeout(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_STALE_LOCK_TIMEOUT");

    sqlite3 *db = fresh_db();
    assert(db);
    config_set(db, "stale_lock_timeout", "120");

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


static void test_agent_dir_resolve(void) {
    char out[256];

    /* Normal case: agent folder is the parent of the workspace. */
    assert(strcmp(agent_dir_resolve("/home/x/.cclaw/agents/bot/workspace", NULL,
                                    out, sizeof(out)),
                  "/home/x/.cclaw/agents/bot") == 0);

    /* Trailing slash on the workspace is tolerated. */
    assert(strcmp(agent_dir_resolve("/a/b/workspace/", NULL, out, sizeof(out)),
                  "/a/b") == 0);

    /* No workspace → anchor to the DB directory's agents/ tree. */
    assert(strcmp(agent_dir_resolve(NULL, "/home/x/.cclaw/cclaw.db",
                                    out, sizeof(out)),
                  "/home/x/.cclaw/agents") == 0);
    assert(strcmp(agent_dir_resolve("", "/home/x/.cclaw/cclaw.db",
                                    out, sizeof(out)),
                  "/home/x/.cclaw/agents") == 0);

    /* Workspace wins over db_path when both are present. */
    assert(strcmp(agent_dir_resolve("/ws/agents/a/workspace", "/db/cclaw.db",
                                    out, sizeof(out)),
                  "/ws/agents/a") == 0);

    /* Nothing usable → relative last-resort. */
    assert(strcmp(agent_dir_resolve(NULL, NULL, out, sizeof(out)),
                  ".cclaw/agents") == 0);

    printf("  PASS: test_agent_dir_resolve\n");
}

int main(void) {
    printf("test_config:\n");
    test_agent_dir_resolve();
    test_defaults();
    test_kv_values();
    test_env_overrides_kv();
    test_kv_secret_fallback();
    test_fallback_providers();
    test_stale_lock_timeout();
    test_system_prompt();
    printf("All config tests passed.\n");
    return 0;
}
