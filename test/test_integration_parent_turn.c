#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include "test_util.h"

#define DB_PATH "/tmp/test_integ_parent_turn.db"

static const char *TOOL_CALL_RESP =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"tc1\",\"type\":\"function\","
    "\"function\":{\"name\":\"memory_create\","
    "\"arguments\":\"{\\\"label\\\":\\\"test\\\",\\\"description\\\":\\\"a test block\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static const char *FINAL_RESP =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"done\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":3}}";

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
    Message user = {.role = ROLE_USER, .content = "create a memory"};
    entry_append_with_turn(db, sid, &user, 1);

    mock_server_enqueue(200, TOOL_CALL_RESP);
    mock_server_enqueue(200, FINAL_RESP);

    Config *cfg = config_load(db);
    AgentSetup setup;
    agent_setup_init(&setup, db, sid, cfg, "default");
    int rc = test_run_session(db, sid, &setup);
    assert(rc == 0);

    /* Verify memory block (container) was created via the tool call */
    MemoryBlock *mb = memory_block_get(db, "default", "test");
    assert(mb);
    assert(strcmp(mb->description, "a test block") == 0);
    memory_block_free(mb);

    /* Verify final assistant response */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_ASSISTANT &&
            branch[i].message.content && strstr(branch[i].message.content, "done"))
            found = 1;
    }
    assert(found);
    entry_branch_free(branch, count);

    agent_setup_destroy(&setup);
    config_free(cfg);
    mock_server_stop();
    db_close(db);
    test_db_clean(DB_PATH);
    printf("PASS test_integration_parent_turn\n");
    return 0;
}
