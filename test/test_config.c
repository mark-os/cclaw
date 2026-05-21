#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_env_only(void) {
    /* Clear any JSON-related env, set required key */
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_TELEGRAM_TOKEN");
    unsetenv("CCLAW_DB_PATH");
    unsetenv("CCLAW_WEB_PORT");
    setenv("OPENROUTER_API_KEY", "sk-test-123", 1);

    Config *cfg = config_load(NULL);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.api_key, "sk-test-123") == 0);
    assert(strcmp(cfg->provider.base_url, "https://openrouter.ai/api/v1") == 0);
    assert(strcmp(cfg->provider.model, "deepseek/deepseek-v4-flash") == 0);
    assert(strcmp(cfg->db_path, "cclaw.db") == 0);
    assert(cfg->web_port == 8080);
    config_free(cfg);
    printf("  PASS: test_env_only\n");
}

static void test_json_parse(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_DB_PATH");
    unsetenv("CCLAW_WEB_PORT");

    /* Write temp config file */
    const char *json =
        "{"
        "  \"provider\": {"
        "    \"base_url\": \"http://localhost:8000/v1\","
        "    \"api_key\": \"json-key\","
        "    \"model\": \"gpt-4\","
        "    \"max_tokens\": 2048"
        "  },"
        "  \"db_path\": \"/tmp/test.db\","
        "  \"web_port\": 9090"
        "}";
    FILE *f = fopen("/tmp/cclaw_test_config.json", "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    Config *cfg = config_load("/tmp/cclaw_test_config.json");
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.base_url, "http://localhost:8000/v1") == 0);
    assert(strcmp(cfg->provider.api_key, "json-key") == 0);
    assert(strcmp(cfg->provider.model, "gpt-4") == 0);
    assert(cfg->provider.max_tokens == 2048);
    assert(strcmp(cfg->db_path, "/tmp/test.db") == 0);
    assert(cfg->web_port == 9090);
    config_free(cfg);
    remove("/tmp/cclaw_test_config.json");
    printf("  PASS: test_json_parse\n");
}

static void test_env_overrides_json(void) {
    /* Write config with one model, then override via env */
    const char *json =
        "{"
        "  \"provider\": {"
        "    \"api_key\": \"json-key\","
        "    \"model\": \"gpt-4\""
        "  }"
        "}";
    FILE *f = fopen("/tmp/cclaw_test_config2.json", "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    setenv("OPENROUTER_API_KEY", "env-key", 1);
    setenv("CCLAW_MODEL", "claude-4", 1);

    Config *cfg = config_load("/tmp/cclaw_test_config2.json");
    assert(cfg != NULL);
    /* Env overrides JSON */
    assert(strcmp(cfg->provider.api_key, "env-key") == 0);
    assert(strcmp(cfg->provider.model, "claude-4") == 0);
    config_free(cfg);

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_MODEL");
    remove("/tmp/cclaw_test_config2.json");
    printf("  PASS: test_env_overrides_json\n");
}

static void test_bad_file(void) {
    Config *cfg = config_load("/tmp/nonexistent_cclaw.json");
    assert(cfg == NULL);
    printf("  PASS: test_bad_file\n");
}

static void test_fallback_providers(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");

    const char *json =
        "{"
        "  \"provider\": {"
        "    \"api_key\": \"primary-key\","
        "    \"model\": \"gpt-4\""
        "  },"
        "  \"providers\": ["
        "    {\"base_url\": \"https://gemini.example.com/v1\", \"api_key\": \"gem-key\", \"model\": \"gemma-4-31b-it\", \"max_tokens\": 1024},"
        "    {\"base_url\": \"https://backup.example.com/v1\", \"api_key\": \"bak-key\", \"model\": \"backup-model\"}"
        "  ]"
        "}";
    FILE *f = fopen("/tmp/cclaw_test_fallback.json", "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    Config *cfg = config_load("/tmp/cclaw_test_fallback.json");
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
    remove("/tmp/cclaw_test_fallback.json");
    printf("  PASS: test_fallback_providers\n");
}

int main(void) {
    printf("test_config:\n");
    test_env_only();
    test_json_parse();
    test_env_overrides_json();
    test_bad_file();
    test_fallback_providers();
    printf("All config tests passed.\n");
    return 0;
}
