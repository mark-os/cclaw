#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <unistd.h>
#include "test_util.h"

#define DB_PATH "/tmp/test_integ_mock_agent.db"
#define FAIL(m) do { fprintf(stderr, "FAIL: %s\n", m); return 1; } while(0)

static const char *MOCK_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello world\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static int test_basic_turn(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    if (!db) FAIL("db_open");

    int port = mock_server_start();
    if (port < 0) { db_close(db); FAIL("mock_server_start"); }

    /* Set env for llm_req */
    char url[64]; snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_AGENT_NAME", "default", 1);
    setenv("CCLAW_STREAM", "0", 1);

    /* Create session with user message */
    db_agent_upsert(db, "default", NULL, NULL);
    int64_t sid = session_create(db, "test", "default", -1, 0);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_turn(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "say hello"};
    entry_append_with_turn(db, sid, &user, 1);

    /* Enqueue mock response */
    mock_server_enqueue(200, MOCK_RESPONSE);

    /* Run */
    Config *cfg = config_load(db);
    AgentSetup setup;
    agent_setup_init(&setup, db, sid, cfg, "default");
    int rc = test_run_session(db, sid, &setup);
    assert(rc == 0);

    /* Verify assistant entry */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count >= 3); /* system + user + assistant */
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_ASSISTANT &&
            branch[i].message.content && strstr(branch[i].message.content, "hello world"))
            found = 1;
    }
    assert(found);
    entry_branch_free(branch, count);

    agent_setup_destroy(&setup);
    config_free(cfg);
    mock_server_stop();
    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS test_basic_turn\n");
    return 0;
}

int main(void) {
    TEST_INIT();
    printf("test_integration_mock_agent:\n");
    if (test_basic_turn()) return 1;
    printf("All mock agent tests passed.\n");
    return 0;
}
