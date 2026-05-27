#include "tool_bootstrap.h"
#include "db.h"
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
    sqlite3 *db = db_open(DB_PATH);
    assert(db != NULL);
    /* Load secret key so encryption works */
    uint8_t key[32];
    if (secret_key_load_or_create(DB_PATH, key) == 0)
        db_set_secret_key(key);
    return db;
}

/* T190: configure_provider with known provider (openrouter) */
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
    assert(strstr(result, "Provider configured") != NULL);
    assert(strstr(result, "openrouter") != NULL);
    free(result);

    /* Verify key stored encrypted */
    char *raw = db_kv_get(db, "provider.api_key");
    assert(raw != NULL);
    assert(strncmp(raw, "enc:", 4) == 0);
    free(raw);

    /* Verify decrypted value matches */
    char *decrypted = db_kv_get_secret(db, "provider.api_key");
    assert(decrypted != NULL);
    assert(strcmp(decrypted, "sk-or-test-key-123") == 0);
    free(decrypted);

    /* Verify base_url and model set */
    char *url = db_kv_get(db, "provider.base_url");
    assert(url != NULL);
    assert(strcmp(url, "https://openrouter.ai/api/v1") == 0);
    free(url);

    char *model = db_kv_get(db, "provider.model");
    assert(model != NULL);
    assert(strcmp(model, "deepseek/deepseek-v4-flash") == 0);
    free(model);

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

/* T190: configure_provider with custom provider + base_url works */
static int test_configure_custom_with_url(void) {
    sqlite3 *db = setup_db();
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
    assert(strstr(result, "Provider configured") != NULL);
    free(result);

    char *url = db_kv_get(db, "provider.base_url");
    assert(strcmp(url, "https://my-llm.example.com/v1") == 0);
    free(url);

    char *model = db_kv_get(db, "provider.model");
    assert(strcmp(model, "my-model-7b") == 0);
    free(model);

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

/* T191: configure_channel telegram stores token encrypted */
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
    assert(strstr(result, "telegram") != NULL);
    assert(strstr(result, "stored securely") != NULL);
    free(result);

    /* Verify token stored encrypted */
    char *raw = db_kv_get(db, "telegram_token");
    assert(raw != NULL);
    assert(strncmp(raw, "enc:", 4) == 0);
    free(raw);

    /* Verify decrypted value matches */
    char *decrypted = db_kv_get_secret(db, "telegram_token");
    assert(decrypted != NULL);
    assert(strcmp(decrypted, "123456:ABC-DEF") == 0);
    free(decrypted);

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

/* T191: configure_channel cli needs no credentials */
static int test_configure_channel_cli(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db};
    tool_configure_channel_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "configure_channel");
    char *result = e->handler("{\"channel_type\":\"cli\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "cli") != NULL);
    assert(strstr(result, "No credentials") != NULL);
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

    ToolEntry *e = tools_lookup(&reg, "configure_channel");
    char *result = e->handler("{\"channel_type\":\"whatsapp\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "unknown") != NULL);
    free(result);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_channel_unknown\n");
    return 0;
}

/* T192: create_agent submits approval request */
static int test_create_agent_basic(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db, .session_id = 1, .agent_name = "bootstrap"};
    assert(tool_create_agent_register(&reg, &ctx) == 0);

    ToolEntry *e = tools_lookup(&reg, "create_agent");
    assert(e != NULL);

    char *result = e->handler(
        "{\"name\":\"helper\",\"model\":\"deepseek/deepseek-v4-flash\","
        "\"system_prompt\":\"You are a helpful assistant.\","
        "\"tools\":[\"shell_exec\",\"file_read\",\"file_write\"],"
        "\"allowed_hosts\":[\"api.example.com\"]}",
        e->user_data);
    assert(result != NULL);
    assert(strstr(result, "approval") != NULL);
    assert(strstr(result, "Waiting") != NULL);
    free(result);

    /* Verify approval row exists */
    int count = 0;
    Approval *list = approval_list_pending(db, &count);
    assert(count == 1);
    assert(strcmp(list[0].type, "create_agent") == 0);
    assert(strstr(list[0].payload, "helper") != NULL);
    approval_list_free(list, count);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_create_agent_basic\n");
    return 0;
}

/* T192: create_agent rejects path separators in name */
static int test_create_agent_invalid_name(void) {
    sqlite3 *db = setup_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db, .session_id = 1, .agent_name = "bootstrap"};
    tool_create_agent_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "create_agent");
    char *result = e->handler("{\"name\":\"../evil\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    result = e->handler("{\"name\":\"a/b\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
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
    ToolBootstrapCtx ctx = {.db = db, .session_id = 1, .agent_name = "bootstrap"};
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
    printf("test_tool_bootstrap (T190, T191, T192):\n");
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
