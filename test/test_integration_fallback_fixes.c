#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include "test_util.h"

#define DB_PATH "/tmp/test_integ_fallback_fixes.db"

static const char *ERROR_RESP = "{\"error\":{\"message\":\"internal error\"}}";

static const char *SUCCESS_RESP =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"recovered\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":3}}";

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
    Message user = {.role = ROLE_USER, .content = "hello"};
    entry_append_with_turn(db, sid, &user, 1);

    /* First request gets 500, second gets 200 */
    mock_server_enqueue(500, ERROR_RESP);
    mock_server_enqueue(200, SUCCESS_RESP);

    Config *cfg = config_load_from_env();
    AgentSetup setup;
    agent_setup_init(&setup, db, sid, cfg, "default", AGENT_SETUP_CLI);
    int rc = test_run_session(db, sid, &setup);
    assert(rc == 0);

    /* Verify assistant response exists */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_ASSISTANT &&
            branch[i].message.content && strstr(branch[i].message.content, "recovered"))
            found = 1;
    }
    assert(found);
    entry_branch_free(branch, count);

    agent_setup_destroy(&setup);
    config_free(cfg);
    mock_server_stop();
    db_close(db);
    test_db_clean(DB_PATH);
    printf("PASS test_integration_fallback_fixes\n");
    return 0;
}
