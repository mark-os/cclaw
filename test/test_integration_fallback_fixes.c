/* A 500 is retried and the retry's answer ends the turn. */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include <unistd.h>

#define DB_PATH "/tmp/test_integ_fallback_fixes.db"

static const char *ERROR_RESP = "{\"error\":{\"message\":\"internal error\"}}";

static const char *SUCCESS_RESP =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"recovered\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":3}}";

int main(void) {
    TEST_INIT();
    alarm(10);
    TestTurn t;
    test_turn_open(&t, DB_PATH, "hello");

    mock_server_enqueue(500, ERROR_RESP);
    mock_server_enqueue(200, SUCCESS_RESP);

    assert(test_turn_run(&t) == 0);
    char *reply = test_turn_reply(&t);
    assert(reply && strstr(reply, "recovered"));
    free(reply);

    test_turn_close(&t);
    printf("PASS test_integration_fallback_fixes\n");
    return 0;
}
