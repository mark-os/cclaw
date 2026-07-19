#define _POSIX_C_SOURCE 200809L
#include "config.h"
#include "db.h"
#include "log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include "test_util.h"

#define TEST_DB "/tmp/test_cclaw_config_env.db"

static void clear_env(void) {
    unsetenv("OPENROUTER_API_KEY");
    unsetenv("CCLAW_PROVIDER_BASE_URL");
    unsetenv("CCLAW_PROVIDER");
    unsetenv("CCLAW_MODEL");
    unsetenv("CCLAW_CONTEXT_WINDOW");
    unsetenv("CCLAW_WORKSPACE");
    unsetenv("CCLAW_MAX_ITERATIONS");
    unsetenv("CCLAW_MAX_HISTORY_TOKENS");
    unsetenv("CCLAW_SHELL_TIMEOUT");
    unsetenv("CCLAW_TOKEN_RATE_LIMIT");
    unsetenv("CCLAW_SAVE_REASONING");
    unsetenv("CCLAW_LOG_LEVEL");
}

static sqlite3 *fresh_db(void) {
    test_db_clean(TEST_DB);
    sqlite3 *db = test_db_open(TEST_DB);
    assert(db);
    return db;
}

static void test_env_defaults(void) {
    clear_env();
    sqlite3 *db = fresh_db();
    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.base_url, "https://openrouter.ai/api/v1") == 0);
    assert(strcmp(cfg->provider.model, "deepseek/deepseek-v4-flash") == 0);
    assert(cfg->provider.max_tokens == 4096);
    assert(cfg->context_window == 128000);
    assert(cfg->max_iterations == 25);
    assert(cfg->shell_timeout == 30);
    assert(cfg->token_rate_limit == 1000000);
    assert(cfg->log_level == LOG_LEVEL_INFO);
    /* workspace defaults next to the DB file */
    assert(strstr(cfg->workspace, "/agents/default/workspace") != NULL);
    config_free(cfg);
    db_close(db);
    test_db_clean(TEST_DB);
    printf("  PASS test_env_defaults\n");
}

static void test_env_openrouter_key(void) {
    clear_env();
    setenv("OPENROUTER_API_KEY", "or-key", 1);
    sqlite3 *db = fresh_db();
    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.api_key, "or-key") == 0);
    config_free(cfg);
    db_close(db);
    test_db_clean(TEST_DB);
    clear_env();
    printf("  PASS test_env_openrouter_key\n");
}

static void test_env_all_overrides(void) {
    clear_env();
    setenv("OPENROUTER_API_KEY", "key1", 1);
    setenv("CCLAW_PROVIDER_BASE_URL", "http://local:8000/v1", 1);
    setenv("CCLAW_MODEL", "gpt-5", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "200000", 1);
    setenv("CCLAW_WORKSPACE", "/tmp/ws", 1);
    setenv("CCLAW_MAX_ITERATIONS", "50", 1);
    setenv("CCLAW_MAX_HISTORY_TOKENS", "10000", 1);
    setenv("CCLAW_SHELL_TIMEOUT", "60", 1);
    setenv("CCLAW_TOKEN_RATE_LIMIT", "500000", 1);
    setenv("CCLAW_SAVE_REASONING", "1", 1);
    setenv("CCLAW_LOG_LEVEL", "trace", 1);

    sqlite3 *db = fresh_db();
    Config *cfg = config_load(db);
    assert(cfg != NULL);
    assert(strcmp(cfg->provider.api_key, "key1") == 0);
    assert(strcmp(cfg->provider.base_url, "http://local:8000/v1") == 0);
    assert(strcmp(cfg->provider.model, "gpt-5") == 0);
    assert(cfg->context_window == 200000);
    assert(strcmp(cfg->workspace, "/tmp/ws") == 0);
    assert(cfg->max_iterations == 50);
    assert(cfg->max_history_tokens == 10000);
    assert(cfg->shell_timeout == 60);
    assert(cfg->token_rate_limit == 500000);
    assert(cfg->save_reasoning == 1);
    assert(cfg->log_level == LOG_LEVEL_TRACE);
    config_free(cfg);
    db_close(db);
    test_db_clean(TEST_DB);
    clear_env();
    printf("  PASS test_env_all_overrides\n");
}

static void test_render_workspace_var(void) {
    clear_env();
    setenv("OPENROUTER_API_KEY", "k", 1);
    setenv("CCLAW_WORKSPACE", "/tmp/render_ws", 1);
    sqlite3 *db = fresh_db();
    Config *cfg = config_load(db);
    assert(cfg != NULL);
    free(cfg->system_prompt);
    cfg->system_prompt = strdup("ws={workspace} sid={session_id} d={date}");
    char *rendered = config_render_system_prompt(cfg, 99);
    assert(rendered != NULL);
    assert(strstr(rendered, "ws=/tmp/render_ws") != NULL);
    assert(strstr(rendered, "sid=99") != NULL);
    /* date is dynamic but should be YYYY-MM-DD format */
    assert(strstr(rendered, "d=20") != NULL);
    free(rendered);
    config_free(cfg);
    db_close(db);
    test_db_clean(TEST_DB);
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

int main(void) {
    TEST_INIT();
    alarm(10);
    printf("test_config_env:\n");
    test_env_defaults();
    test_env_openrouter_key();
    test_env_all_overrides();
    test_render_workspace_var();
    test_render_no_template_vars();
    printf("All config_env tests passed.\n");
    return 0;
}
