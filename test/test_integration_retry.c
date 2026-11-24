/* A 429 is retried and the retry's answer ends the turn. */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include <unistd.h>

#define DB_PATH "/tmp/test_integ_retry.db"

static const char *OK_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"ok\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":1}}";

int main(void) {
    TEST_INIT();
    alarm(10);
    TestTurn t;
    test_turn_open(&t, DB_PATH, "hi");

    mock_server_enqueue(429, "{\"error\":{\"message\":\"rate limited\"}}");
    mock_server_enqueue(200, OK_RESPONSE);

    assert(test_turn_run(&t) == 0);
    assert(mock_server_request_count() == 2);
    char *reply = test_turn_reply(&t);
    assert(reply && strcmp(reply, "ok") == 0);
    free(reply);

    test_turn_close(&t);
    printf("PASS test_integration_retry\n");
    return 0;
}
