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

/* configure_provider applies config (providers row + encrypted kv) */
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

    /* Key stored encrypted in secrets table under the canonical env-var name */
    char *key = db_secret_get_system(db, "OPENROUTER_API_KEY");
    assert(key && strcmp(key, "sk-or-test-key-123") == 0);
    free(key);

    /* Raw value in secrets table has enc: prefix */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT value FROM secrets WHERE name='OPENROUTER_API_KEY'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *raw = (const char *)sqlite3_column_text(s, 0);
    assert(raw && strncmp(raw, "enc:", 4) == 0);
    sqlite3_finalize(s);

    /* Provider row upserted with defaults */
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

/* configure_provider with custom provider requires base_url */
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

/* configure_provider with custom provider + base_url applies config */
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

    char *key = db_secret_get_system(db, "CUSTOM_API_KEY");
    assert(key && strcmp(key, "mykey") == 0);
    free(key);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_configure_custom_with_url\n");
    return 0;
}

/* missing api_key returns error */
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

/* create_agent parks an approval carrying the full definition */
static int test_create_agent_parks(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "Bootstrap", NULL, NULL);
    int64_t sid = session_create(db, "t", "Bootstrap", -1, 0);
    assert(sid > 0);
    ToolRegistry reg;
    tools_init(&reg);
    ToolBootstrapCtx ctx = {.db = db, .session_id = sid, .agent_name = "Bootstrap",
                            .current_tool_call_id = "call_1"};
    assert(tool_create_agent_register(&reg, &ctx) == 0);

    ToolEntry *e = tools_lookup(&reg, "create_agent");
    assert(e != NULL);

    char *result = e->handler(
        "{\"name\":\"Helper\",\"description\":\"helps\","
        "\"system_prompt\":\"You are a helpful assistant.\"}",
        e->user_data);
    assert(result == NULL); /* parked */

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.name') FROM approvals"
        " WHERE session_id=?1 AND tool_name='create_agent' AND state='pending'",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "Helper") == 0);
    sqlite3_finalize(s);

    /* Agent must NOT exist yet — creation happens only on approval. */
    assert(sqlite3_prepare_v2(db, "SELECT 1 FROM agents WHERE name='Helper'",
                              -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    cleanup();
    printf("  PASS: test_create_agent_parks\n");
    return 0;
}

/* create_agent rejects non-PascalCase names */
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

/* create_agent requires name field */
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
    printf("test_tool_bootstrap:\n");
    int rc = 0;
    rc |= test_configure_openrouter();
    rc |= test_configure_custom_requires_base_url();
    rc |= test_configure_custom_with_url();
    rc |= test_configure_missing_key();
    rc |= test_create_agent_parks();
    rc |= test_create_agent_invalid_name();
    rc |= test_create_agent_missing_name();
    return rc;
}
