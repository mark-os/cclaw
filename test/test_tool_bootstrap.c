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

int main(void) {
    printf("test_tool_bootstrap (T190):\n");
    int rc = 0;
    rc |= test_configure_openrouter();
    rc |= test_configure_custom_requires_base_url();
    rc |= test_configure_custom_with_url();
    rc |= test_configure_missing_key();
    return rc;
}
