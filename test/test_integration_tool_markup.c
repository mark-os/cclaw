#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include <unistd.h>

#define DB_PATH "/tmp/test_integ_tool_markup.db"

/* Bug 4 end-to-end: DeepSeek's tool call comes back as raw DSML markup inside
 * `reasoning` with no content and no tool_calls. Wire-legal 2xx, so nothing
 * used to fire — the turn ended in silence. It must now be classified,
 * retried, and answered by the retry. */


static const char *DSML_RESPONSE =
    "{\"provider\":\"StreamLake\","
    "\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":null,"
    "\"reasoning\":\"<｜DSML｜tool_calls><｜DSML｜invoke name=js_eval>1+1\"},"
    "\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":367,\"total_tokens\":467}}";

static const char *GOOD_RESPONSE =
    "{\"provider\":\"DeepInfra\","
    "\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"It is 2.\"},"
    "\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":4,\"total_tokens\":104}}";

int main(void) {
    TEST_INIT();
    alarm(20);   /* one same-model backoff step (1s) sits inside this run */
    TestTurn t;
    test_turn_open(&t, DB_PATH, "what is 1+1");

    mock_server_enqueue(200, DSML_RESPONSE);
    mock_server_enqueue(200, GOOD_RESPONSE);

    assert(test_turn_run(&t) == 0);

    /* The retry's answer is what the session ends on: no silent turn. */
    char *reply = test_turn_reply(&t);
    assert(reply && strcmp(reply, "It is 2.") == 0);
    free(reply);

    /* Both attempts are in the archive, each tagged with its upstream: the
     * query that answers "which backend keeps doing this". */
    assert(test_qi(t.db, "SELECT COUNT(*) FROM llm_responses"
                         " WHERE status='tool_markup' AND upstream_provider='StreamLake';") == 1);
    assert(test_qi(t.db, "SELECT COUNT(*) FROM llm_responses"
                         " WHERE status='ok' AND upstream_provider='DeepInfra';") == 1);

    test_turn_close(&t);
    printf("PASS test_integration_tool_markup\n");
    return 0;
}
