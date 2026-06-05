#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static void clear_env(void) {
    unsetenv("CCLAW_PROVIDER_API_KEY_ENV");
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_PROVIDER_BASE_URL");
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_MAX_TOKENS");
    unsetenv("CCLAW_CONTEXT_WINDOW");
    unsetenv("CCLAW_WORKSPACE");
    unsetenv("CCLAW_AGENT_DB");
    unsetenv("CCLAW_MAX_ITERATIONS");
    unsetenv("CCLAW_MAX_HISTORY_TOKENS");
    unsetenv("CCLAW_SHELL_TIMEOUT");
    unsetenv("CCLAW_TOKEN_RATE_LIMIT");
    unsetenv("CCLAW_SAVE_REASONING");
    unsetenv("CCLAW_SAVE_USAGE");
    unsetenv("CCLAW_SAVE_LOGPROBS");
    unsetenv("CCLAW_LOG_LEVEL");
    unsetenv("CCLAW_CACHE_HINTS");
}

static void test_env_defaults(void) {
    clear_env();
    Config *cfg = config_load_from_env();
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.api_key, "") == 0);
    assert(strcmp(cfg->provider.base_url, "https://openrouter.ai/api/v1") == 0);
    assert(strcmp(cfg->provider.model, "deepseek/deepseek-v4-flash") == 0);
    assert(cfg->provider.max_tokens == 4096);
    assert(cfg->provider.context_window == 65536);
    assert(strcmp(cfg->workspace, ".cclaw/agents/default/workspace") == 0);
    assert(strcmp(cfg->db_path, ".cclaw/agents/default/agent.db") == 0);
    assert(cfg->max_iterations == 25);
    assert(cfg->shell_timeout == 30);
    assert(cfg->token_rate_limit == 1000000);
    assert(cfg->log_level == LOG_LEVEL_INFO);
    config_free(cfg);
    printf("  PASS test_env_defaults\n");
}

static void test_env_injected_key_priority(void) {
    clear_env();
    setenv("CCLAW_PROVIDER_API_KEY_ENV", "OPENROUTER_API_KEY", 1);
    setenv("OPENROUTER_API_KEY", "injected-key", 1);
    Config *cfg = config_load_from_env();
    assert(cfg != NULL);
    /* CCLAW_PROVIDER_API_KEY_ENV indirection takes priority */
    assert(strcmp(cfg->provider.api_key, "injected-key") == 0);
    config_free(cfg);
    clear_env();
    printf("  PASS test_env_injected_key_priority\n");
}

static void test_env_openrouter_fallback(void) {
    clear_env();
    setenv("OPENROUTER_API_KEY", "or-key", 1);
    Config *cfg = config_load_from_env();
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.api_key, "or-key") == 0);
    config_free(cfg);
    clear_env();
    printf("  PASS test_env_openrouter_fallback\n");
}

static void test_env_all_overrides(void) {
    clear_env();
    setenv("CCLAW_PROVIDER_API_KEY_ENV", "OPENROUTER_API_KEY", 1);
    setenv("OPENROUTER_API_KEY", "key1", 1);
    setenv("CCLAW_PROVIDER_BASE_URL", "http://local:8000/v1", 1);
    setenv("CCLAW_MODEL", "gpt-5", 1);
    setenv("CCLAW_MAX_TOKENS", "8192", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "200000", 1);
    setenv("CCLAW_WORKSPACE", "/tmp/ws", 1);
    setenv("CCLAW_AGENT_DB", "/tmp/a.db", 1);
    setenv("CCLAW_MAX_ITERATIONS", "50", 1);
    setenv("CCLAW_MAX_HISTORY_TOKENS", "10000", 1);
    setenv("CCLAW_SHELL_TIMEOUT", "60", 1);
    setenv("CCLAW_TOKEN_RATE_LIMIT", "500000", 1);
    setenv("CCLAW_SAVE_REASONING", "1", 1);
    setenv("CCLAW_SAVE_USAGE", "1", 1);
    setenv("CCLAW_SAVE_LOGPROBS", "1", 1);
    setenv("CCLAW_LOG_LEVEL", "trace", 1);

    Config *cfg = config_load_from_env();
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.api_key, "key1") == 0);
    assert(strcmp(cfg->provider.base_url, "http://local:8000/v1") == 0);
    assert(strcmp(cfg->provider.model, "gpt-5") == 0);
    assert(cfg->provider.max_tokens == 8192);
    assert(cfg->provider.context_window == 200000);
    assert(strcmp(cfg->workspace, "/tmp/ws") == 0);
    assert(strcmp(cfg->db_path, "/tmp/a.db") == 0);
    assert(cfg->max_iterations == 50);
    assert(cfg->max_history_tokens == 10000);
    assert(cfg->shell_timeout == 60);
    assert(cfg->token_rate_limit == 500000);
    assert(cfg->save_reasoning == 1);
    assert(cfg->save_usage == 1);
    assert(cfg->save_logprobs == 1);
    assert(cfg->log_level == LOG_LEVEL_TRACE);
    config_free(cfg);
    clear_env();
    printf("  PASS test_env_all_overrides\n");
}

static void test_render_workspace_var(void) {
    clear_env();
    setenv("OPENROUTER_API_KEY", "k", 1);
    Config *cfg = config_load_from_env();
    assert(cfg != NULL);
    free(cfg->system_prompt);
    cfg->system_prompt = strdup("ws={workspace} sid={session_id} d={date}");
    char *rendered = config_render_system_prompt(cfg, 99);
    assert(rendered != NULL);
    assert(strstr(rendered, "ws=.cclaw/agents/default/workspace") != NULL);
    assert(strstr(rendered, "sid=99") != NULL);
    /* date is dynamic but should be YYYY-MM-DD format */
    assert(strstr(rendered, "d=20") != NULL);
    free(rendered);
    config_free(cfg);
    clear_env();
    printf("  PASS test_render_workspace_var\n");
}

static void test_render_no_template_vars(void) {
    clear_env();
    Config cfg = {0};
    cfg.workspace = "ws";
    cfg.system_prompt = strdup("plain prompt no vars");
    char *rendered = config_render_system_prompt(&cfg, 1);
    assert(rendered != NULL);
    assert(strcmp(rendered, "plain prompt no vars") == 0);
    free(rendered);
    free(cfg.system_prompt);
    printf("  PASS test_render_no_template_vars\n");
}

static void test_cache_hints_env(void) {
    clear_env();
    setenv("OPENROUTER_API_KEY", "k", 1);

    /* default = auto */
    Config *cfg = config_load_from_env();
    assert(cfg->provider.cache_hints == CACHE_HINTS_AUTO);
    config_free(cfg);

    /* on */
    setenv("CCLAW_CACHE_HINTS", "on", 1);
    cfg = config_load_from_env();
    assert(cfg->provider.cache_hints == CACHE_HINTS_ON);
    config_free(cfg);

    /* off */
    setenv("CCLAW_CACHE_HINTS", "off", 1);
    cfg = config_load_from_env();
    assert(cfg->provider.cache_hints == CACHE_HINTS_OFF);
    config_free(cfg);

    /* gemini-native */
    setenv("CCLAW_CACHE_HINTS", "gemini-native", 1);
    cfg = config_load_from_env();
    assert(cfg->provider.cache_hints == CACHE_HINTS_GEMINI_NATIVE);
    config_free(cfg);

    clear_env();
    printf("  PASS test_cache_hints_env\n");
}

int main(void) {
    alarm(10);
    printf("test_config_env:\n");
    test_env_defaults();
    test_env_injected_key_priority();
    test_env_openrouter_fallback();
    test_env_all_overrides();
    test_render_workspace_var();
    test_render_no_template_vars();
    test_cache_hints_env();
    printf("All config_env tests passed.\n");
    return 0;
}
