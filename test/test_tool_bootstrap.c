#define _POSIX_C_SOURCE 200809L
#include "tool_bootstrap.h"
#include "db.h"
#include "test_util.h"
#include "secret.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_tool_bootstrap.db"

static void cleanup(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    unlink("/tmp/test_tool_bootstrap.db.key");
}

static sqlite3 *setup_db(void) {
    cleanup();
    /* Prevent env var from seeding provider.api_key during db_open */
    unsetenv("OPENROUTER_API_KEY");
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    /* Load secret key so encryption works */
    uint8_t key[32];
    if (secret_key_load_or_create(DB_PATH, key) == 0)
        db_set_secret_key(key);
    return db;
}

/* T190: configure_provider applies config (providers row + encrypted kv) */
static int test_configure_openrouter(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    assert(tool_configure_provider_register(&reg, &ctx) == 0);

    ToolEntry *e = tools_lookup(&reg, "configure_provider");
    assert(e != NULL);

    char *result = e->handler(
        "{\"provider\":\"openrouter\",\"api_key\":\"sk-or-test-key-123\"}",
        e->user_data);
    assert(result != NULL);
    assert(strstr(result, "config applied:") != NULL);
    assert(strstr(result, "configure_provider") != NULL);
    free(result);

    /* Key stored encrypted in kv under the canonical env-var name */
    char *key = db_kv_get_secret(db, "OPENROUTER_API_KEY");
    assert(key && strcmp(key, "sk-or-test-key-123") == 0);
    free(key);
    char *raw = db_kv_get(db, "OPENROUTER_API_KEY");
    assert(raw && strncmp(raw, "enc:", 4) == 0);
    free(raw);

    /* Provider row upserted with defaults */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT base_url, api_key_env FROM providers WHERE name='openrouter'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "https://openrouter.ai/api/v1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "OPENROUTER_API_KEY") == 0);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_openrouter\n");
    return 0;
}

/* T190: configure_provider with custom provider requires base_url */
static int test_configure_custom_requires_base_url(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    tool_configure_provider_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "configure_provider");
    char *result = e->handler(
        "{\"provider\":\"custom\",\"api_key\":\"key123\"}",
        e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "base_url") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_custom_requires_base_url\n");
    return 0;
}

/* T190: configure_provider with custom provider + base_url applies config */
static int test_configure_custom_with_url(void) {
    sqlite3 *db = setup_db();
    db_seed_defaults(db);
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    tool_configure_provider_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "configure_provider");
    char *result = e->handler(
        "{\"provider\":\"custom\",\"api_key\":\"mykey\","
        "\"base_url\":\"https://my-llm.example.com/v1\","
        "\"model\":\"my-model-7b\"}",
        e->user_data);
    assert(result != NULL);
    assert(strstr(result, "config applied:") != NULL);
    free(result);

    /* Custom provider appended after existing ones — priority 0 untouched */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT base_url FROM providers WHERE priority=0", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "https://openrouter.ai/api/v1") == 0);
    sqlite3_finalize(s);

    sqlite3_prepare_v2(db, "SELECT base_url, default_model, api_key_env FROM providers WHERE name='custom'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "https://my-llm.example.com/v1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "my-model-7b") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "CUSTOM_API_KEY") == 0);
    sqlite3_finalize(s);

    char *key = db_kv_get_secret(db, "CUSTOM_API_KEY");
    assert(key && strcmp(key, "mykey") == 0);
    free(key);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_custom_with_url\n");
    return 0;
}

/* T190: missing api_key returns error */
static int test_configure_missing_key(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    tool_configure_provider_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "configure_provider");
    char *result = e->handler("{\"provider\":\"openrouter\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_missing_key\n");
    return 0;
}

/* T191: configure_channel telegram returns sentinel */
static int test_configure_channel_telegram(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    assert(tool_configure_channel_register(&reg, &ctx) == 0);

    ToolEntry *e = tools_lookup(&reg, "configure_channel");
    assert(e != NULL);

    char *result = e->handler(
        "{\"channel_type\":\"telegram\",\"bot_token\":\"123456:ABC-DEF\"}",
        e->user_data);
    assert(result != NULL);
    assert(strstr(result, "config applied:") != NULL);
    assert(strstr(result, "configure_channel") != NULL);
    free(result);

    /* V76: Tool does NOT write token — value still empty default */

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_channel_telegram\n");
    return 0;
}

/* T191: configure_channel telegram requires bot_token */
static int test_configure_channel_telegram_no_token(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    tool_configure_channel_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "configure_channel");
    char *result = e->handler("{\"channel_type\":\"telegram\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "bot_token") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_channel_telegram_no_token\n");
    return 0;
}

/* T191: configure_channel cli returns sentinel */
static int test_configure_channel_cli(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    tool_configure_channel_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "configure_channel");
    char *result = e->handler("{\"channel_type\":\"cli\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "config applied:") != NULL);
    assert(strstr(result, "configure_channel") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_channel_cli\n");
    return 0;
}

/* T191: configure_channel unknown type returns error */
static int test_configure_channel_unknown(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    tool_configure_channel_register(&reg, &ctx);

    /* Custom type without binary_path → error */
    ToolEntry *e = tools_lookup(&reg, "configure_channel");
    char *result = e->handler("{\"channel_type\":\"whatsapp\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "binary_path") != NULL);
    free(result);

    /* Custom type with binary_path → sentinel (success) */
    result = e->handler("{\"channel_type\":\"whatsapp\",\"binary_path\":\"/usr/bin/wa\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "config applied:") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_channel_unknown\n");
    return 0;
}

/* T192/T201: create_agent returns sentinel (daemon handles after reap) */
static int test_create_agent_basic(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db, .session_id = 1, .agent_name = "Bootstrap"};
    assert(tool_create_agent_register(&reg, &ctx) == 0);

    ToolEntry *e = tools_lookup(&reg, "create_agent");
    assert(e != NULL);

    char *result = e->handler(
        "{\"name\":\"Helper\",\"model\":\"deepseek/deepseek-v4-flash\","
        "\"system_prompt\":\"You are a helpful assistant.\","
        "\"tools\":[\"shell_exec\",\"file_read\",\"file_write\"],"
        "\"allowed_hosts\":[\"api.example.com\"]}",
        e->user_data);
    assert(result != NULL);
    assert(strstr(result, "config applied:") != NULL);
    assert(strstr(result, "create_agent") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_create_agent_basic\n");
    return 0;
}

/* T192: create_agent rejects non-PascalCase names */
static int test_create_agent_invalid_name(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db, .session_id = 1, .agent_name = "Bootstrap"};
    tool_create_agent_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "create_agent");
    char *result = e->handler("{\"name\":\"../evil\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "agent name must be PascalCase") != NULL);
    free(result);

    result = e->handler("{\"name\":\"a/b\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "agent name must be PascalCase") != NULL);
    free(result);

    result = e->handler("{\"name\":\"lowercase\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "agent name must be PascalCase") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_create_agent_invalid_name\n");
    return 0;
}

/* T192: create_agent requires name field */
static int test_create_agent_missing_name(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db, .session_id = 1, .agent_name = "Bootstrap"};
    tool_create_agent_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "create_agent");
    char *result = e->handler("{\"model\":\"gpt-4\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "name") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_create_agent_missing_name\n");
    return 0;
}

int main(void) {
    printf("test_tool_bootstrap (T190, T191, T192, T205):\n");
    int rc = 0;
    rc |= test_configure_openrouter();
    rc |= test_configure_custom_requires_base_url();
    rc |= test_configure_custom_with_url();
    rc |= test_configure_missing_key();
    rc |= test_configure_channel_telegram();
    rc |= test_configure_channel_telegram_no_token();
    rc |= test_configure_channel_cli();
    rc |= test_configure_channel_unknown();
    rc |= test_create_agent_basic();
    rc |= test_create_agent_invalid_name();
    rc |= test_create_agent_missing_name();
    return rc;
}
