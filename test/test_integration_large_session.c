#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include "test_util.h"

#define DB_PATH "/tmp/test_integ_large_session.db"

static const char *MOCK_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":500,\"completion_tokens\":2}}";

int main(void) {
    TEST_INIT();
    alarm(10);
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    int port = mock_server_start();
    assert(port > 0);

    char url[64]; snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_AGENT_NAME", "default", 1);
    setenv("CCLAW_STREAM", "0", 1);

    db_agent_upsert(db, "default", NULL, NULL);
    int64_t sid = session_create(db, "test", "default", -1, 0);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_turn(db, sid, &sys, 1);

    /* Create 50 user+assistant pairs */
    for (int i = 0; i < 50; i++) {
        Message u = {.role = ROLE_USER, .content = "msg"};
        entry_append_with_turn(db, sid, &u, 1);
        Message a = {.role = ROLE_ASSISTANT, .content = "reply"};
        entry_append_with_turn(db, sid, &a, 1);
    }

    /* Add final user message */
    Message user = {.role = ROLE_USER, .content = "one more"};
    entry_append_with_turn(db, sid, &user, 1);

    mock_server_enqueue(200, MOCK_RESPONSE);

    Config *cfg = config_load(db);
    AgentSetup setup;
    agent_setup_init(&setup, db, sid, cfg, "default", AGENT_SETUP_CLI);
    int rc = test_run_session(db, sid, &setup);
    assert(rc == 0);

    agent_setup_destroy(&setup);
    config_free(cfg);
    mock_server_stop();
    db_close(db);
    test_db_clean(DB_PATH);
    printf("PASS test_integration_large_session\n");
    return 0;
}
