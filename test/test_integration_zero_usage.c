/* A reply with content but zero usage is an answer, not a provider glitch. */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include <unistd.h>

#define DB_PATH "/tmp/test_integ_zero_usage.db"

static const char *ZERO_USAGE_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":0,\"completion_tokens\":0}}";

int main(void) {
    TEST_INIT();
    alarm(10);
    TestTurn t;
    test_turn_open(&t, DB_PATH, "hi");

    mock_server_enqueue(200, ZERO_USAGE_RESPONSE);

    assert(test_turn_run(&t) == 0);
    char *reply = test_turn_reply(&t);
    assert(reply && strcmp(reply, "hi") == 0);
    free(reply);

    test_turn_close(&t);
    printf("PASS test_integration_zero_usage\n");
    return 0;
}
