#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include "test_util.h"

/* Bug 4 end-to-end: DeepSeek's tool call comes back as raw DSML markup inside
 * `reasoning` with no content and no tool_calls. Wire-legal 2xx, so nothing
 * used to fire — the turn ended in silence. It must now be classified,
 * retried, and answered by the retry. */

#define DB_PATH "/tmp/test_integ_tool_markup.db"

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
    Message sys = {.role = ROLE_SYSTEM, .content = "sys"};
    entry_append_with_iteration(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "what is 1+1"};
    entry_append_with_iteration(db, sid, &user, 1);

    mock_server_enqueue(200, DSML_RESPONSE);
    mock_server_enqueue(200, GOOD_RESPONSE);

    Config *cfg = config_load(db);
    AgentSetup setup;
    agent_setup_init(&setup, db, sid, cfg, "default");
    assert(test_run_session(db, sid, &setup) == 0);

    /* The retry's answer is what the session ends on — no silent turn. */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    int answered = 0;
    for (int i = 0; i < count; i++)
        if (branch[i].message.role == ROLE_ASSISTANT && branch[i].message.content &&
            strcmp(branch[i].message.content, "It is 2.") == 0)
            answered = 1;
    entry_branch_free(branch, count);
    assert(answered);

    /* Both attempts are in the archive, each tagged with its upstream — the
     * query that answers "which backend keeps doing this". */
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM llm_responses"
        " WHERE status='tool_markup' AND upstream_provider='StreamLake';", 0, -1) == 1);
    assert(db_scalar_i64(db,
        "SELECT COUNT(*) FROM llm_responses"
        " WHERE status='ok' AND upstream_provider='DeepInfra';", 0, -1) == 1);

    agent_setup_destroy(&setup);
    config_free(cfg);
    mock_server_stop();
    db_close(db);
    test_db_clean(DB_PATH);
    printf("PASS test_integration_tool_markup\n");
    return 0;
}
