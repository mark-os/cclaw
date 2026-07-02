#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "admin_api.h"
#include "db.h"
#include "test_util.h"
#include <sqlite3.h>
#include "agent_config.h"

#define DB_PATH "/tmp/test_admin_api.db"

static const uint8_t TEST_KEY[32] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32
};

static sqlite3 *setup_db(void) {
    unlink(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    db_set_secret_key(TEST_KEY);
    return db;
}

static void test_key_env_name(void) {
    assert(strcmp(admin_key_env_name("openrouter"), "OPENROUTER_API_KEY") == 0);
    assert(strcmp(admin_key_env_name("gemini"), "GEMINI_API_KEY") == 0);
    assert(admin_key_env_name("unknown") == NULL);
    assert(admin_key_env_name(NULL) == NULL);
    printf("  PASS: test_key_env_name\n");
}

static void test_set_key_known_provider(void) {
    sqlite3 *db = setup_db();
    assert(admin_set_key(db, "openrouter", "sk-test-123") == 0);

    /* Stored encrypted under the canonical env-var name */
    char *val = db_kv_get_secret(db, "OPENROUTER_API_KEY");
    assert(val && strcmp(val, "sk-test-123") == 0);
    free(val);
    char *raw = db_kv_get(db, "OPENROUTER_API_KEY");
    assert(raw && strncmp(raw, "enc:", 4) == 0);
    free(raw);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS: test_set_key_known_provider\n");
}

static void test_set_key_custom(void) {
    sqlite3 *db = setup_db();
    assert(admin_set_key(db, "custom", "MY_VAR=secret") == 0);

    char *val = db_kv_get_secret(db, "MY_VAR");
    assert(val && strcmp(val, "secret") == 0);
    free(val);

    /* Malformed custom values rejected */
    assert(admin_set_key(db, "custom", "no-equals") == -1);
    assert(admin_set_key(db, "custom", "=leading-eq") == -1);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS: test_set_key_custom\n");
}

static void test_set_model_primary(void) {
    sqlite3 *db = setup_db();
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,default_model,priority)"
        " VALUES('openrouter','https://openrouter.ai/api/v1','OPENROUTER_API_KEY','old-model',0);",
        NULL, NULL, NULL);

    assert(admin_set_model(db, 0, "new-model") == 0);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT default_model FROM providers WHERE priority=0", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *val = (const char *)sqlite3_column_text(s, 0);
    assert(val && strcmp(val, "new-model") == 0);
    sqlite3_finalize(s);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS: test_set_model_primary\n");
}

static void test_set_endpoint_primary(void) {
    sqlite3 *db = setup_db();
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,priority)"
        " VALUES('openrouter','https://old.api/v1','OPENROUTER_API_KEY',0);",
        NULL, NULL, NULL);

    assert(admin_set_endpoint(db, 0, "https://new.api/v1") == 0);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT base_url FROM providers WHERE priority=0", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *val = (const char *)sqlite3_column_text(s, 0);
    assert(val && strcmp(val, "https://new.api/v1") == 0);
    sqlite3_finalize(s);

    /* Reject non-http URLs */
    assert(admin_set_endpoint(db, 0, "ftp://bad.url") == -1);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS: test_set_endpoint_primary\n");
}

static void test_set_model_fallback(void) {
    sqlite3 *db = setup_db();
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,default_model,priority)"
        " VALUES('openrouter','https://openrouter.ai/api/v1','OPENROUTER_API_KEY','primary',0);",
        NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,default_model,priority)"
        " VALUES('fallback','https://fb.api/v1','FB_KEY','fb-old',1);",
        NULL, NULL, NULL);

    assert(admin_set_model(db, 1, "fb-new") == 0);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT default_model FROM providers WHERE priority=1", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *val = (const char *)sqlite3_column_text(s, 0);
    assert(val && strcmp(val, "fb-new") == 0);
    sqlite3_finalize(s);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS: test_set_model_fallback\n");
}

static void test_host_management(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "testagent", NULL, NULL);

    assert(admin_add_host(db, "testagent", "example.com") == 0);
    assert(admin_add_host(db, "testagent", "api.openai.com") == 0);

    AgentCaps caps;
    agent_caps_load(db, "testagent", &caps);
    assert(caps.host_count == 2);
    agent_caps_free(&caps);

    assert(admin_remove_host(db, "testagent", "example.com") == 0);
    agent_caps_load(db, "testagent", &caps);
    assert(caps.host_count == 1);
    assert(strcmp(caps.hosts[0], "api.openai.com") == 0);
    agent_caps_free(&caps);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS: test_host_management\n");
}

static void test_list_providers(void) {
    sqlite3 *db = setup_db();
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,default_model,priority)"
        " VALUES('openrouter','https://openrouter.ai/api/v1','OPENROUTER_API_KEY','deepseek-v4',0);",
        NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,default_model,priority)"
        " VALUES('openai','https://api.openai.com/v1','OPENAI_API_KEY','gpt-4o',1);",
        NULL, NULL, NULL);

    AdminProvider *providers = NULL;
    size_t count = 0;
    assert(admin_list_providers(db, &providers, &count) == 0);
    assert(count == 2);
    assert(strcmp(providers[0].model, "deepseek-v4") == 0);
    assert(strcmp(providers[1].model, "gpt-4o") == 0);
    admin_providers_free(providers, count);

    db_close(db);
    unlink(DB_PATH);
    printf("  PASS: test_list_providers\n");
}

int main(void) {
    printf("test_admin_api:\n");
    test_key_env_name();
    test_set_key_known_provider();
    test_set_key_custom();
    test_set_model_primary();
    test_set_endpoint_primary();
    test_set_model_fallback();
    test_host_management();
    test_list_providers();
    printf("all admin_api tests passed\n");
    return 0;
}
