#include "daemon.h"
#include "db.h"
#include "config.h"
#include "secret.h"
#include "agent_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define DB_PATH "/tmp/test_daemon_config.db"
#define AGENTS_DIR "/tmp/test_daemon_config_agents"

static void cleanup(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    unlink("/tmp/test_daemon_config.db.key");
    /* Remove agents dir */
    system("rm -rf " AGENTS_DIR);
}

static sqlite3 *setup_db(void) {
    cleanup();
    mkdir(AGENTS_DIR, 0755);
    sqlite3 *db = db_open_cclaw(DB_PATH);
    assert(db != NULL);
    uint8_t key[32];
    if (secret_key_load_or_create(DB_PATH, key) == 0)
        db_set_secret_key(key);
    return db;
}

/* T205/V79: configure_provider — daemon stores API key + base_url + model */
static int test_apply_configure_provider(void) {
    sqlite3 *db = setup_db();
    Config cfg = {0};

    char *result = daemon_apply_config(&cfg, db, "test-agent",
        "configure_provider",
        "{\"provider\":\"openrouter\",\"api_key\":\"sk-or-test-123\"}");
    assert(result != NULL);
    assert(strstr(result, "provider configured") != NULL);
    assert(strstr(result, "openrouter") != NULL);
    free(result);

    /* Verify key stored encrypted */
    char *raw = db_kv_get(db, "provider.api_key");
    assert(raw != NULL);
    assert(strncmp(raw, "enc:", 4) == 0);
    free(raw);

    /* Verify decrypted value */
    char *decrypted = db_kv_get_secret(db, "provider.api_key");
    assert(decrypted != NULL);
    assert(strcmp(decrypted, "sk-or-test-123") == 0);
    free(decrypted);

    /* Verify base_url and model from known provider defaults */
    char *url = db_kv_get(db, "provider.base_url");
    assert(url != NULL);
    assert(strcmp(url, "https://openrouter.ai/api/v1") == 0);
    free(url);

    char *model = db_kv_get(db, "provider.model");
    assert(model != NULL);
    assert(strcmp(model, "deepseek/deepseek-v4-flash") == 0);
    free(model);

    db_close(db);
    cleanup();
    printf("  PASS: test_apply_configure_provider\n");
    return 0;
}

/* T205: configure_provider with custom base_url override */
static int test_apply_configure_provider_custom(void) {
    sqlite3 *db = setup_db();
    Config cfg = {0};

    char *result = daemon_apply_config(&cfg, db, "test-agent",
        "configure_provider",
        "{\"provider\":\"custom\",\"api_key\":\"mykey\","
        "\"base_url\":\"https://my.llm/v1\",\"model\":\"my-7b\"}");
    assert(result != NULL);
    assert(strstr(result, "provider configured") != NULL);
    free(result);

    char *url = db_kv_get(db, "provider.base_url");
    assert(url != NULL);
    assert(strcmp(url, "https://my.llm/v1") == 0);
    free(url);

    char *model = db_kv_get(db, "provider.model");
    assert(model != NULL);
    assert(strcmp(model, "my-7b") == 0);
    free(model);

    db_close(db);
    cleanup();
    printf("  PASS: test_apply_configure_provider_custom\n");
    return 0;
}

/* T205/V79: configure_channel telegram — stores token + binds channel */
static int test_apply_configure_channel_telegram(void) {
    sqlite3 *db = setup_db();
    Config cfg = {0};

    char *result = daemon_apply_config(&cfg, db, "my-agent",
        "configure_channel",
        "{\"channel_type\":\"telegram\",\"bot_token\":\"123:ABC\"}");
    assert(result != NULL);
    assert(strstr(result, "telegram") != NULL);
    free(result);

    /* Verify token stored encrypted */
    char *decrypted = db_kv_get_secret(db, "telegram_token");
    assert(decrypted != NULL);
    assert(strcmp(decrypted, "123:ABC") == 0);
    free(decrypted);

    /* V69: Verify channel binding */
    char *bound = db_channel_binding_get(db, "telegram", "default");
    assert(bound != NULL);
    assert(strcmp(bound, "my-agent") == 0);
    free(bound);

    db_close(db);
    cleanup();
    printf("  PASS: test_apply_configure_channel_telegram\n");
    return 0;
}

/* T205/V79: configure_channel cli — binds channel, no credentials */
static int test_apply_configure_channel_cli(void) {
    sqlite3 *db = setup_db();
    Config cfg = {0};

    char *result = daemon_apply_config(&cfg, db, "cli-agent",
        "configure_channel",
        "{\"channel_type\":\"cli\"}");
    assert(result != NULL);
    assert(strstr(result, "cli") != NULL);
    free(result);

    char *bound = db_channel_binding_get(db, "cli", "default");
    assert(bound != NULL);
    assert(strcmp(bound, "cli-agent") == 0);
    free(bound);

    db_close(db);
    cleanup();
    printf("  PASS: test_apply_configure_channel_cli\n");
    return 0;
}

/* T205/V79: create_agent — daemon creates agent dir + DB rows */
static int test_apply_create_agent(void) {
    sqlite3 *db = setup_db();
    Config cfg = {0};

    /* Temporarily chdir so agent_config_create uses our test agents dir */
    char cwd[1024];
    getcwd(cwd, sizeof(cwd));
    chdir("/tmp");

    /* Create the agents dir that agent_config_create expects */
    mkdir("agents", 0755);

    char *result = daemon_apply_config(&cfg, db, "bootstrap-agent",
        "create_agent",
        "{\"name\":\"helper\",\"model\":\"gpt-4\","
        "\"system_prompt\":\"You help.\","
        "\"tools\":[\"shell_exec\",\"file_read\"]}");
    assert(result != NULL);
    assert(strstr(result, "agent created") != NULL);
    assert(strstr(result, "helper") != NULL);
    free(result);

    /* Verify agent dir created */
    struct stat st;
    assert(stat("agents/helper", &st) == 0);
    assert(S_ISDIR(st.st_mode));
    assert(stat("agents/helper/workspace", &st) == 0);

    /* Verify system.md written */
    assert(stat("agents/helper/system.md", &st) == 0);

    /* Verify agent_config in cclaw.db */
    AgentConfig *ac = agent_config_load_db(db, "helper");
    assert(ac != NULL);
    assert(ac->model != NULL);
    assert(strcmp(ac->model, "gpt-4") == 0);
    agent_config_free(ac);

    /* Cleanup */
    system("rm -rf /tmp/agents");
    chdir(cwd);
    db_close(db);
    cleanup();
    printf("  PASS: test_apply_create_agent\n");
    return 0;
}

/* T205: unknown tool_name returns error */
static int test_apply_unknown_tool(void) {
    sqlite3 *db = setup_db();
    Config cfg = {0};

    char *result = daemon_apply_config(&cfg, db, "agent",
        "unknown_tool", "{}");
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    assert(strstr(result, "unknown") != NULL);
    free(result);

    db_close(db);
    cleanup();
    printf("  PASS: test_apply_unknown_tool\n");
    return 0;
}

/* T205: invalid JSON args returns error */
static int test_apply_invalid_json(void) {
    sqlite3 *db = setup_db();
    Config cfg = {0};

    char *result = daemon_apply_config(&cfg, db, "agent",
        "configure_provider", "not json");
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    db_close(db);
    cleanup();
    printf("  PASS: test_apply_invalid_json\n");
    return 0;
}

int main(void) {
    printf("test_daemon_config (T205, V79):\n");
    int rc = 0;
    rc |= test_apply_configure_provider();
    rc |= test_apply_configure_provider_custom();
    rc |= test_apply_configure_channel_telegram();
    rc |= test_apply_configure_channel_cli();
    rc |= test_apply_create_agent();
    rc |= test_apply_unknown_tool();
    rc |= test_apply_invalid_json();
    printf("All daemon_apply_config tests passed.\n");
    return rc;
}
