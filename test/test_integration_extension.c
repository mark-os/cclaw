#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>

#define DB_PATH "/tmp/test_integ_extension.db"

static const char *MOCK_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":2}}";

int main(void) {
    alarm(10);
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int port = mock_server_start();
    assert(port > 0);

    char url[64]; snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_DB", DB_PATH, 1);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_AGENT_NAME", "default", 1);
    setenv("CCLAW_STREAM", "0", 1);

    db_agent_upsert(db, "default", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "default", -1, 0);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append(db, sid, &sys);
    Message user = {.role = ROLE_USER, .content = "test"};
    entry_append(db, sid, &user);

    mock_server_enqueue(200, MOCK_RESPONSE);

    Config *cfg = config_load_from_env();
    AgentSetup setup;
    int init_rc = agent_setup_init(&setup, db, sid, cfg, "default", NULL, 0, AGENT_SETUP_CLI);
    assert(init_rc == 0);
    int rc = test_run_session(db, sid, &setup);
    assert(rc == LLM_EXIT_STOP);

    agent_setup_destroy(&setup);
    config_free(cfg);
    mock_server_stop();
    db_close(db);
    unlink(DB_PATH);
    printf("PASS test_integration_extension\n");
    return 0;
}
