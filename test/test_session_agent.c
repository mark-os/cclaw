#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "agent_config.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEST_DB "/tmp/test_session_agent.sqlite"
#define FAIL(msg) do { fprintf(stderr, "  FAIL %s: %s\n", __func__, msg); return; } while(0)

static sqlite3 *setup(void) {
    unlink(TEST_DB);
    return test_db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    unlink(TEST_DB);
}

/* T79: session_create stores agent_name, session_get_agent_name retrieves it */
static void test_create_with_agent_name(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", "coder", -1, 0);
    assert(sid > 0);

    char *name = session_get_agent_name(db, sid);
    assert(name != NULL);
    assert(strcmp(name, "coder") == 0);
    free(name);

    teardown(db);
    printf("  PASS test_create_with_agent_name\n");
}

/* T79: NULL agent_name → session_get_agent_name returns NULL */
static void test_create_without_agent_name(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    char *name = session_get_agent_name(db, sid);
    assert(name == NULL);

    teardown(db);
    printf("  PASS test_create_without_agent_name\n");
}

/* T79: session_list includes agent_name */
static void test_list_includes_agent_name(void) {
    sqlite3 *db = setup();
    session_create(db, "s1", "writer", -1, 0);
    session_create(db, "s2", NULL, -1, 0);

    int count = 0;
    Session *list = session_list(db, &count);
    assert(count == 2);
    assert(list[0].agent_name != NULL);
    assert(strcmp(list[0].agent_name, "writer") == 0);
    assert(list[1].agent_name == NULL);

    session_list_free(list, count);
    teardown(db);
    printf("  PASS test_list_includes_agent_name\n");
}

/* T79: agent_config_merge with agent_name produces correct effective config */
static void test_merge_at_run_time(void) {
    /* Create agent dir with config */
    mkdir("/tmp/test_agents", 0755);
    mkdir("/tmp/test_agents/mybot", 0755);
    FILE *f = fopen("/tmp/test_agents/mybot/agent.json", "w");
    fprintf(f, "{\"model\":\"gpt-4\",\"max_iterations\":5}");
    fclose(f);

    Config global = {0};
    global.provider.base_url = strdup("https://api.example.com");
    global.provider.api_key = strdup("key");
    global.provider.model = strdup("default-model");
    global.max_iterations = 25;

    AgentConfig *ac = agent_config_load("/tmp/test_agents", "mybot");
    assert(ac != NULL);
    assert(strcmp(ac->model, "gpt-4") == 0);

    Config *merged = agent_config_merge(&global, ac);
    assert(merged != NULL);
    assert(strcmp(merged->provider.model, "gpt-4") == 0);
    assert(merged->max_iterations == 5);
    /* base_url/api_key inherited from global */
    assert(strcmp(merged->provider.base_url, "https://api.example.com") == 0);

    config_free(merged);
    agent_config_free(ac);
    free(global.provider.base_url);
    free(global.provider.api_key);
    free(global.provider.model);

    unlink("/tmp/test_agents/mybot/agent.json");
    rmdir("/tmp/test_agents/mybot");
    rmdir("/tmp/test_agents");
    printf("  PASS test_merge_at_run_time\n");
}

int main(void) {
    printf("test_session_agent:\n");
    test_create_with_agent_name();
    test_create_without_agent_name();
    test_list_includes_agent_name();
    test_merge_at_run_time();
    printf("All session↔agent binding tests passed.\n");
    return 0;
}
