/* A tool call in the turn runs, and the turn ends on the answer after it. */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include <unistd.h>

#define DB_PATH "/tmp/test_integ_parent_turn.db"

static const char *TOOL_CALL_RESP =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"tool_calls\":[{\"id\":\"tc1\",\"type\":\"function\","
    "\"function\":{\"name\":\"memory_block_create\","
    "\"arguments\":\"{\\\"label\\\":\\\"test\\\",\\\"description\\\":\\\"a test block\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5}}";

static const char *FINAL_RESP =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"done\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":3}}";

int main(void) {
    TEST_INIT();
    alarm(10);
    TestTurn t;
    test_turn_open(&t, DB_PATH, "create a memory");

    mock_server_enqueue(200, TOOL_CALL_RESP);
    mock_server_enqueue(200, FINAL_RESP);

    assert(test_turn_run(&t) == 0);

    /* The memory block (container) was created by the tool call. */
    MemoryBlock *mb = memory_block_get(t.db, "default", "test");
    assert(mb);
    assert(strcmp(mb->description, "a test block") == 0);
    memory_block_free(mb);

    char *reply = test_turn_reply(&t);
    assert(reply && strstr(reply, "done"));
    free(reply);

    test_turn_close(&t);
    printf("PASS test_integration_parent_turn\n");
    return 0;
}
