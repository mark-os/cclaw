/* An empty completion with real usage is a (blank) answer; a zero-usage
 * empty completion is a provider glitch, retried on the same model. */
#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include <unistd.h>

#define DB_PATH "/tmp/test_integ_empty_response.db"

static const char *EMPTY_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":0,\"total_tokens\":5}}";

/* Zero-usage empty stop — the provider-glitch shape (GMICloud 2026-07-09):
 * 200 OK, content null, no usage. Must be retried on the same model. */
static const char *GLITCH_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null},"
    "\"finish_reason\":\"stop\"}]}";

static const char *OK_RESPONSE =
    "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"recovered\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":1}}";

int main(void) {
    TEST_INIT();
    alarm(10);
    TestTurn t;
    test_turn_open(&t, DB_PATH, "hi");

    mock_server_enqueue(200, EMPTY_RESPONSE);
    assert(test_turn_run(&t) == 0);
    assert(db_scalar_i64(t.db, "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=2",
                         t.sid, -1) >= 1);

    /* Zero-usage empty completion: retried on the same model, then the next
     * attempt's real answer lands. One retry sleep (1s) is expected. */
    int reqs_before = mock_server_request_count();
    Message user2 = {.role = ROLE_USER, .content = "again"};
    entry_append_with_iteration(t.db, t.sid, &user2, 1);
    mock_server_enqueue(200, GLITCH_RESPONSE);
    mock_server_enqueue(200, OK_RESPONSE);

    assert(test_turn_run(&t) == 0);
    assert(mock_server_request_count() == reqs_before + 2);

    /* The turn ended with the retried answer, not an error entry. */
    char *reply = test_turn_reply(&t);
    assert(reply && strcmp(reply, "recovered") == 0);
    free(reply);

    /* The glitch is archived with the request we sent (for `cclaw resp N req`). */
    assert(test_qi(t.db, "SELECT COUNT(*) FROM llm_responses WHERE status='empty'"
                         " AND request_body IS NOT NULL"
                         " AND iteration_id=(SELECT MAX(iteration_id) FROM llm_responses);") == 1);

    test_turn_close(&t);
    printf("PASS test_integration_empty_response\n");
    return 0;
}
