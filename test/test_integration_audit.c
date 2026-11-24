/* The provider's usage counts land on the assistant entry. */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include <unistd.h>

#define DB_PATH "/tmp/test_integ_audit.db"

static const char *MOCK_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hi\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":7}}";

int main(void) {
    TEST_INIT();
    alarm(10);
    TestTurn t;
    test_turn_open(&t, DB_PATH, "hello");

    mock_server_enqueue(200, MOCK_RESPONSE);

    assert(test_turn_run(&t) == 0);
    assert(db_scalar_i64(t.db, "SELECT COUNT(*) FROM entries WHERE session_id=?"
                         " AND role=2 AND usage_in=42 AND usage_out=7", t.sid, -1) == 1);

    test_turn_close(&t);
    printf("PASS test_integration_audit\n");
    return 0;
}
