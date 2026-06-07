#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>

#define DB_PATH "/tmp/test_integ_parent_turn.db"

static const char *TOOL_CALL_RESP =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"tc1\",\"type\":\"function\","
    "\"function\":{\"name\":\"memory_create\","
    "\"arguments\":\"{\\\"label\\\":\\\"test\\\",\\\"description\\\":\\\"a test block\\\",\\\"value\\\":\\\"hello\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static const char *FINAL_RESP =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"done\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":3}}";

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
    Message user = {.role = ROLE_USER, .content = "create a memory"};
    entry_append(db, sid, &user);

    mock_server_enqueue(200, TOOL_CALL_RESP);
    mock_server_enqueue(200, FINAL_RESP);

    Config *cfg = config_load_from_env();
    AgentSetup setup;
    agent_setup_init(&setup, db, sid, cfg, "default", NULL, 0, AGENT_SETUP_CLI);
    int rc = test_run_session(db, sid, &setup);
    assert(rc == LLM_EXIT_STOP);

    /* Verify memory block was created */
    MemoryBlock *mb = memory_block_get(db, "default", "test");
    assert(mb);
    assert(strcmp(mb->value, "hello") == 0);
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
    unlink(DB_PATH);
    printf("PASS test_integration_parent_turn\n");
    return 0;
}
