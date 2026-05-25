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
    assert(strcmp(cfg->db_path, "cclaw.db") == 0 ||
           (strlen(cfg->db_path) > 9 &&
            strcmp(cfg->db_path + strlen(cfg->db_path) - 9, "/cclaw.db") == 0));
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

static void test_stale_lock_timeout(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_STALE_LOCK_TIMEOUT");

    const char *json =
        "{"
        "  \"provider\": {\"api_key\": \"k\"},"
        "  \"stale_lock_timeout\": 120"
        "}";
    FILE *f = fopen("/tmp/cclaw_test_slt.json", "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    Config *cfg = config_load("/tmp/cclaw_test_slt.json");
    assert(cfg != NULL);
    assert(cfg->stale_lock_timeout == 120);
    config_free(cfg);

    /* Env override */
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    setenv("CCLAW_STALE_LOCK_TIMEOUT", "60", 1);
    cfg = config_load(NULL);
    assert(cfg != NULL);
    assert(cfg->stale_lock_timeout == 60);
    config_free(cfg);

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_STALE_LOCK_TIMEOUT");
    remove("/tmp/cclaw_test_slt.json");
    printf("  PASS: test_stale_lock_timeout\n");
}

static void test_system_prompt(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_SYSTEM_PROMPT");

    /* Test default (no system_prompt in config) */
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    Config *cfg = config_load(NULL);
    assert(cfg != NULL);
    assert(cfg->system_prompt == NULL);
    char *rendered = config_render_system_prompt(cfg, 42);
    assert(rendered != NULL);
    /* Default prompt should contain key sections */
    assert(strstr(rendered, "CClaw") != NULL);
    assert(strstr(rendered, "Tool Call Style") != NULL);
    assert(strstr(rendered, "Execution Bias") != NULL);
    free(rendered);
    config_free(cfg);

    /* Test with template vars */
    const char *json =
        "{"
        "  \"provider\": {\"api_key\": \"k\"},"
        "  \"system_prompt\": \"Agent session={session_id} date={date}\""
        "}";
    FILE *f = fopen("/tmp/cclaw_test_sysprompt.json", "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    cfg = config_load("/tmp/cclaw_test_sysprompt.json");
    assert(cfg != NULL);
    assert(cfg->system_prompt != NULL);
    rendered = config_render_system_prompt(cfg, 99);
    assert(rendered != NULL);
    /* Check session_id was substituted */
    assert(strstr(rendered, "session=99") != NULL);
    /* Check date was substituted (YYYY-MM-DD format, 10 chars) */
    assert(strstr(rendered, "date=") != NULL);
    char *dp = strstr(rendered, "date=") + 5;
    assert(dp[4] == '-' && dp[7] == '-'); /* YYYY-MM-DD */
    free(rendered);
    config_free(cfg);

    /* Test env override */
    setenv("CCLAW_SYSTEM_PROMPT", "env prompt {session_id}", 1);
    cfg = config_load(NULL);
    assert(cfg != NULL);
    rendered = config_render_system_prompt(cfg, 7);
    assert(strstr(rendered, "env prompt 7") != NULL);
    free(rendered);
    config_free(cfg);

    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_SYSTEM_PROMPT");
    remove("/tmp/cclaw_test_sysprompt.json");
    printf("  PASS: test_system_prompt\n");
}

static void test_admin_chat_ids(void) {
    unsetenv("OPENROUTER_API_KEY");

    const char *json =
        "{"
        "  \"provider\": {\"api_key\": \"k\"},"
        "  \"admin_chat_ids\": [111222333, 444555666]"
        "}";
    FILE *f = fopen("/tmp/cclaw_test_admin.json", "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    Config *cfg = config_load("/tmp/cclaw_test_admin.json");
    assert(cfg != NULL);
    assert(cfg->admin_chat_id_count == 2);
    assert(cfg->admin_chat_ids[0] == 111222333);
    assert(cfg->admin_chat_ids[1] == 444555666);
    config_free(cfg);

    /* No admin_chat_ids → count 0, pointer NULL */
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    cfg = config_load(NULL);
    assert(cfg != NULL);
    assert(cfg->admin_chat_id_count == 0);
    assert(cfg->admin_chat_ids == NULL);
    config_free(cfg);

    unsetenv("OPENROUTER_API_KEY");
    remove("/tmp/cclaw_test_admin.json");
    printf("  PASS: test_admin_chat_ids\n");
}

/* T142: config_update_model tests */
static void test_update_model(void) {
    const char *path = "/tmp/cclaw_test_update_model.json";
    const char *json =
        "{\n"
        "  \"provider\": {\"base_url\": \"https://openrouter.ai/api/v1\", \"model\": \"old-model\"},\n"
        "  \"providers\": [{\"base_url\": \"https://gemini.api\", \"model\": \"gemini-old\"}]\n"
        "}";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    /* Update primary provider model */
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    assert(config_update_model(path, 0, "new-model") == 0);
    Config *cfg = config_load(path);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.model, "new-model") == 0);
    /* Fallback unchanged */
    assert(cfg->fallback_count == 1);
    assert(strcmp(cfg->fallback_providers[0].model, "gemini-old") == 0);
    config_free(cfg);

    /* Update fallback provider model */
    assert(config_update_model(path, 1, "gemini-new") == 0);
    cfg = config_load(path);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.model, "new-model") == 0);
    assert(strcmp(cfg->fallback_providers[0].model, "gemini-new") == 0);
    config_free(cfg);

    /* Invalid index fails */
    assert(config_update_model(path, 5, "bad") == -1);

    /* NULL path fails */
    assert(config_update_model(NULL, 0, "x") == -1);

    unsetenv("OPENROUTER_API_KEY");
    remove(path);
    printf("  PASS: test_update_model\n");
}

/* T143: config_update_endpoint tests */
static void test_update_endpoint(void) {
    const char *path = "/tmp/cclaw_test_update_endpoint.json";
    const char *json =
        "{\n"
        "  \"provider\": {\"base_url\": \"https://openrouter.ai/api/v1\", \"model\": \"m\"},\n"
        "  \"providers\": [{\"base_url\": \"https://old.api/v1\", \"model\": \"g\"}]\n"
        "}";
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(json, f);
    fclose(f);

    setenv("OPENROUTER_API_KEY", "sk-test", 1);

    /* Update primary endpoint */
    assert(config_update_endpoint(path, 0, "https://new.api/v1") == 0);
    Config *cfg = config_load(path);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.base_url, "https://new.api/v1") == 0);
    config_free(cfg);

    /* Update fallback endpoint */
    assert(config_update_endpoint(path, 1, "http://localhost:8080/v1") == 0);
    cfg = config_load(path);
    assert(cfg != NULL);
    assert(strcmp(cfg->fallback_providers[0].base_url, "http://localhost:8080/v1") == 0);
    config_free(cfg);

    /* Invalid URL (no http/https) fails */
    assert(config_update_endpoint(path, 0, "ftp://bad.url") == -1);
    assert(config_update_endpoint(path, 0, "not-a-url") == -1);

    /* Invalid index fails */
    assert(config_update_endpoint(path, 5, "https://x.com") == -1);

    /* NULL args fail */
    assert(config_update_endpoint(NULL, 0, "https://x.com") == -1);
    assert(config_update_endpoint(path, 0, NULL) == -1);

    unsetenv("OPENROUTER_API_KEY");
    remove(path);
    printf("  PASS: test_update_endpoint\n");
}

int main(void) {
    printf("test_config:\n");
    test_env_only();
    test_json_parse();
    test_env_overrides_json();
    test_bad_file();
    test_fallback_providers();
    test_stale_lock_timeout();
    test_system_prompt();
    test_admin_chat_ids();
    test_update_model();
    test_update_endpoint();
    printf("All config tests passed.\n");
    return 0;
}
