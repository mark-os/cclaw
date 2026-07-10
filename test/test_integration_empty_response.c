#define _POSIX_C_SOURCE 200809L
#include "test_run_session.h"
#include "mock_server.h"
#include <assert.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include "test_util.h"

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
    alarm(10);
    unlink(DB_PATH);
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
    entry_append_with_turn(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "hi"};
    entry_append_with_turn(db, sid, &user, 1);

    mock_server_enqueue(200, EMPTY_RESPONSE);

    Config *cfg = config_load_from_env();
    AgentSetup setup;
    agent_setup_init(&setup, db, sid, cfg, "default", AGENT_SETUP_CLI);
    int rc = test_run_session(db, sid, &setup);
    assert(rc == 0);

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(count >= 3);
    int found = 0;
    for (int i = 0; i < count; i++)
        if (branch[i].message.role == ROLE_ASSISTANT) found = 1;
    assert(found);
    entry_branch_free(branch, count);

    /* ── Zero-usage empty completion: retried on the same model, then the
     * next attempt's real answer lands. One retry sleep (1s) is expected. */
    int reqs_before = mock_server_request_count();
    Message user2 = {.role = ROLE_USER, .content = "again"};
    entry_append_with_turn(db, sid, &user2, 1);
    mock_server_enqueue(200, GLITCH_RESPONSE);
    mock_server_enqueue(200, OK_RESPONSE);

    int rc2 = test_run_session(db, sid, &setup);
    assert(rc2 == 0);
    assert(mock_server_request_count() == reqs_before + 2);

    /* The turn ended with the retried answer, not an error entry. */
    count = 0;
    branch = session_get_branch(db, sid, &count);
    const char *last_assistant = NULL;
    for (int i = 0; i < count; i++)
        if (branch[i].message.role == ROLE_ASSISTANT && branch[i].message.content)
            last_assistant = branch[i].message.content;
    assert(last_assistant && strcmp(last_assistant, "recovered") == 0);
    entry_branch_free(branch, count);

    /* The glitch is archived with the request we sent (for `cclaw resp N req`). */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM llm_responses WHERE status='empty'"
        " AND request_body IS NOT NULL"
        " AND turn_id=(SELECT MAX(turn_id) FROM llm_responses);", -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW && sqlite3_column_int(st, 0) == 1);
    sqlite3_finalize(st);

    agent_setup_destroy(&setup);
    config_free(cfg);
    mock_server_stop();
    db_close(db);
    unlink(DB_PATH);
    printf("PASS test_integration_empty_response\n");
    return 0;
}
