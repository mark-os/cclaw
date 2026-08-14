/* Integration test — llm_req() core function.
 * Verifies that llm_req correctly:
 * 1. Plans context from DB entries
 * 2. Sends LLM request to mock server
 * 3. Writes assistant entry to DB
 * 4. Returns 0 on success, -1 on error */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "llm_proc.h"
#include "db.h"
#include "test_util.h"
#include "config.h"
#include "mock_server.h"

static int s_port;
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static const char *STOP_RESPONSE =
    "{\"id\":\"chatcmpl-1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
    "\"content\":\"Hello there!\"},\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":3,\"total_tokens\":13}}";

static const char *TOOL_CALL_RESPONSE =
    "{\"id\":\"chatcmpl-2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
    "\"content\":null,\"tool_calls\":[{\"id\":\"call_abc\",\"type\":\"function\","
    "\"function\":{\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"foo.txt\\\"}\"}}]},"
    "\"finish_reason\":\"tool_calls\"}],"
    "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8,\"total_tokens\":28}}";

static void set_test_env(const char *db_path) {
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "128000", 1);
    setenv("CCLAW_DB_PATH", db_path, 1);
    setenv("CCLAW_STREAM", "0", 1);
    setenv("CCLAW_MAX_ITERATIONS", "5", 1);
    setenv("CCLAW_AUTO_RECALL", "0", 1);
}

/* Test: llm_req returns 0 and writes assistant entry on stop response */
static void test_llm_req_stop(void) {
    TEST(llm_req_stop);

    const char *db_path = "/tmp/cclaw_llm_req_test.db";
    test_db_clean(db_path);

    mock_server_reset();
    mock_server_enqueue(200, STOP_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_iteration(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "Hello"};
    entry_append_with_iteration(db, sid, &user, 1);

    set_test_env(db_path);

    int rc = llm_req(db, NULL, sid, 0);
    if (rc != 0) { db_close(db); test_db_clean(db_path); FAIL("expected rc==0"); }

    /* Verify entry was written */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    if (count != 3) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 3 entries, got %d", count);
        entry_branch_free(branch, count); db_close(db); test_db_clean(db_path); FAIL(msg);
    }
    if (branch[2].message.role != ROLE_ASSISTANT ||
        !branch[2].message.content ||
        strcmp(branch[2].message.content, "Hello there!") != 0) {
        entry_branch_free(branch, count); db_close(db); test_db_clean(db_path);
        FAIL("wrong assistant content");
    }
    entry_branch_free(branch, count);
    db_close(db); test_db_clean(db_path);
    PASS();
}

/* Routing skips a model whose provider key resolves nowhere.
 *
 * Regression: load_candidates joined models→providers with no key-availability
 * predicate, so a keyless provider was offered to the request path, went out
 * with an empty "Authorization: Bearer", took a 401, and degraded a healthy
 * model for 300s. A fresh install holding only OPENAI_API_KEY hit this on turn
 * 1, because the seeded priority-0 openrouter row has no key. It is also what
 * made config_load's key-availability scan a no-op for chat routing: the scan
 * chose cfg->provider, which routing only consults for its synthetic fallback
 * candidate. */
static void test_llm_req_skips_keyless_provider(void) {
    TEST(llm_req_skips_keyless_provider);

    const char *db_path = "/tmp/cclaw_llm_req_keyless.db";
    test_db_clean(db_path);

    mock_server_reset();
    mock_server_enqueue(200, STOP_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    test_seed_agent(db, "Router");

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);

    /* Both providers point at the mock, so the only difference that can steer
     * the request is whether the key resolves. */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env,"
        " default_model, priority) VALUES"
        " ('nokey-prov', ?1, 'openai', 'CCLAW_TEST_ABSENT_KEY', 'model-a', 0),"
        " ('keyed-prov', ?1, 'openai', 'CCLAW_TEST_PRESENT_KEY', 'model-b', 10)",
        -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, url, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);

    assert(sqlite3_exec(db,
        "INSERT INTO models(id, provider_name, model, priority) VALUES"
        " ('nokey/model-a','nokey-prov','model-a',0),"
        " ('keyed/model-b','keyed-prov','model-b',10);", NULL, NULL, NULL) == SQLITE_OK);

    /* Pin the agent at the keyless model — the strongest form of the bug:
     * an explicit primary_model that cannot possibly authenticate. */
    assert(sqlite3_exec(db, "UPDATE agents SET primary_model='nokey/model-a'"
                            " WHERE name='Router';", NULL, NULL, NULL) == SQLITE_OK);

    int64_t sid = session_create(db, "keyless", "Router", -1, 0);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_iteration(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "Hello"};
    entry_append_with_iteration(db, sid, &user, 1);

    set_test_env(db_path);
    setenv("CCLAW_TEST_PRESENT_KEY", "sk-present", 1);
    unsetenv("CCLAW_TEST_ABSENT_KEY");

    int rc = llm_req(db, NULL, sid, 0);

    int reqs = mock_server_request_count();
    const char *req = mock_server_last_request_body();
    int used_keyed = (req && strstr(req, "model-b") != NULL);
    int used_keyless = (req && strstr(req, "model-a") != NULL);

    unsetenv("CCLAW_TEST_PRESENT_KEY");
    db_close(db); test_db_clean(db_path);

    if (rc != 0) FAIL("expected rc==0");
    /* One attempt only: the keyless model was never tried, so no 401 and no
     * degradation of a model that was fine. */
    if (reqs != 1) { char m[64]; snprintf(m, sizeof(m), "expected 1 request, got %d", reqs); FAIL(m); }
    if (used_keyless) FAIL("routed to the keyless model");
    if (!used_keyed) FAIL("did not route to the keyed model");
    PASS();
}

/* A8: a dropped candidate leaves a trace, through the machinery that already
 * exists for models that fail at request time.
 *
 * The 2026-08-10 incident's silent half — a phantom api_key_env dropped both
 * gateway models pre-request, so nothing recorded errors, the rows stayed
 * 'healthy', and every request quietly served from a different provider.
 * Dropping now marks the row degraded on the healthy→degraded transition
 * (exactly one operator notice, like the error paths), and un-marks it when
 * the key resolves again. */
static void test_dropped_candidate_degrades_and_recovers(void) {
    TEST(dropped_candidate_degrades_and_recovers);

    const char *db_path = "/tmp/cclaw_llm_req_a8.db";
    test_db_clean(db_path);

    mock_server_reset();
    mock_server_enqueue(200, STOP_RESPONSE);
    mock_server_enqueue(200, STOP_RESPONSE);
    mock_server_enqueue(200, STOP_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    test_seed_agent(db, "Router");

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env, priority)"
        " VALUES ('nokey-prov', ?1, 'openai', 'CCLAW_TEST_ABSENT_KEY', 0),"
        "        ('keyed-prov', ?1, 'openai', '', 10)",
        -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, url, -1, SQLITE_STATIC);
    assert(sqlite3_step(st) == SQLITE_DONE);
    sqlite3_finalize(st);
    assert(sqlite3_exec(db,
        "INSERT INTO models(id, provider_name, model, priority) VALUES"
        " ('model-a@nokey-prov','nokey-prov','model-a',0),"
        " ('model-b@keyed-prov','keyed-prov','model-b',10);"
        "UPDATE agents SET primary_model='model-a@nokey-prov',"
        " secondary_model='model-b@keyed-prov' WHERE name='Router';",
        NULL, NULL, NULL) == SQLITE_OK);

    int64_t sid = session_create(db, "a8", "Router", -1, 0);
    /* A channel-bound session is what makes the operator notice observable. */
    assert(sqlite3_exec(db, "UPDATE sessions SET channel_name='testch',"
                            " chat_id='c1' WHERE id=(SELECT MAX(id) FROM sessions)",
                        NULL, NULL, NULL) == SQLITE_OK);
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_iteration(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "Hello"};
    entry_append_with_iteration(db, sid, &user, 1);

    set_test_env(db_path);
    unsetenv("CCLAW_TEST_ABSENT_KEY");

    assert(llm_req(db, NULL, sid, 0) == 0);
    /* Degraded by configuration: marked, but with no cooldown — that is what
     * separates it from an error-driven degradation. */
    int degraded = 0, notices = 0;
    sqlite3_stmt *q;
    assert(sqlite3_prepare_v2(db,
        "SELECT (SELECT COUNT(*) FROM models WHERE id='model-a@nokey-prov'"
        "         AND status='degraded' AND degraded_until IS NULL),"
        "       (SELECT COUNT(*) FROM channel_outbox)", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    degraded = sqlite3_column_int(q, 0);
    notices = sqlite3_column_int(q, 1);
    sqlite3_finalize(q);
    if (!degraded) { db_close(db); test_db_clean(db_path); FAIL("candidate not degraded"); }
    if (notices != 1) { db_close(db); test_db_clean(db_path); FAIL("expected exactly 1 notice"); }

    /* Second turn, same missing key: no second notice (transition-guarded). */
    Message u2 = {.role = ROLE_USER, .content = "Again"};
    entry_append_with_iteration(db, sid, &u2, 1);
    assert(llm_req(db, NULL, sid, 0) == 0);
    assert(sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM channel_outbox",
                              -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    notices = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    if (notices != 1) { db_close(db); test_db_clean(db_path); FAIL("notice repeated"); }

    /* The key comes back — recovery, mirroring model_stat_success. */
    setenv("CCLAW_TEST_ABSENT_KEY", "sk-present", 1);
    Message u3 = {.role = ROLE_USER, .content = "Once more"};
    entry_append_with_iteration(db, sid, &u3, 1);
    assert(llm_req(db, NULL, sid, 0) == 0);
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM models WHERE id='model-a@nokey-prov'"
        " AND status='healthy'", -1, &q, NULL) == SQLITE_OK);
    assert(sqlite3_step(q) == SQLITE_ROW);
    int healthy = sqlite3_column_int(q, 0);
    sqlite3_finalize(q);
    unsetenv("CCLAW_TEST_ABSENT_KEY");
    db_close(db); test_db_clean(db_path);
    if (!healthy) FAIL("candidate did not recover");
    PASS();
}

/* Test: llm_req returns 0 and writes tool_calls on tool_calls response */
static void test_llm_req_tool_calls(void) {
    TEST(llm_req_tool_calls);

    const char *db_path = "/tmp/cclaw_llm_req_tc_test.db";
    test_db_clean(db_path);

    mock_server_reset();
    mock_server_enqueue(200, TOOL_CALL_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_iteration(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "Read foo.txt"};
    entry_append_with_iteration(db, sid, &user, 1);

    set_test_env(db_path);

    int rc = llm_req(db, NULL, sid, 0);
    if (rc != 0) { db_close(db); test_db_clean(db_path); FAIL("expected rc==0"); }

    /* Verify tool_calls written */
    sqlite3_stmt *stmt;
    int tc_count = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tool_calls WHERE session_id=?;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, sid);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            tc_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    db_close(db); test_db_clean(db_path);
    if (tc_count != 1) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected 1 tool_call, got %d", tc_count);
        FAIL(msg);
    }
    PASS();
}

/* Test: llm_req returns -1 on server error */
static void test_llm_req_error(void) {
    TEST(llm_req_error);

    const char *db_path = "/tmp/cclaw_llm_req_err_test.db";
    test_db_clean(db_path);

    mock_server_reset();
    mock_server_enqueue(500, "{\"error\":\"internal\"}");

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_iteration(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "Hello"};
    entry_append_with_iteration(db, sid, &user, 1);

    set_test_env(db_path);

    int rc = llm_req(db, NULL, sid, 0);
    if (rc != -1) {
        char msg[64]; snprintf(msg, sizeof(msg), "expected rc==-1, got %d", rc);
        db_close(db); test_db_clean(db_path); FAIL(msg);
    }

    db_close(db); test_db_clean(db_path);
    PASS();
}

int main(void) {
    TEST_INIT();
    s_port = mock_server_start();
    if (s_port <= 0) {
        fprintf(stderr, "FAIL: could not start mock server\n");
        return 1;
    }

    printf("test_integration_llm_proc:\n");
    test_llm_req_stop();
    test_llm_req_skips_keyless_provider();
    test_dropped_candidate_degrades_and_recovers();
    test_llm_req_tool_calls();
    test_llm_req_error();

    mock_server_stop();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
