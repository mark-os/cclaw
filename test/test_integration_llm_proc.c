/* T299: integration test — cclaw llm subcommand.
 * Verifies the LLM child subprocess correctly:
 * 1. Plans context from DB entries
 * 2. Sends LLM request to mock server
 * 3. Writes assistant entry to DB
 * 4. Exits with correct code (0=stop, 10=tool_calls) */
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

/* Test: LLM child returns 0 on stop response with no tool_calls */
static void test_llm_child_stop(void) {
    TEST(llm_child_stop);

    const char *db_path = "/tmp/cclaw_llm_child_test.db";
    unlink(db_path);

    mock_server_reset();
    mock_server_enqueue(200, STOP_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_turn(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "Hello"};
    entry_append_with_turn(db, sid, &user, 1);
    db_close(db);

    /* Set env vars as the parent would */
    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "128000", 1);
    setenv("CCLAW_DB", db_path, 1);
    setenv("CCLAW_STREAM", "0", 1);
    setenv("CCLAW_MAX_ITERATIONS", "5", 1);
    setenv("CCLAW_AUTO_RECALL", "0", 1);

    int rc = llm_proc_main(sid);
    if (rc != LLM_EXIT_STOP) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected exit 0, got %d", rc);
        FAIL(msg);
    }

    /* Verify entry was written */
    db = test_db_open(db_path);
    assert(db);
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    if (count != 3) { /* sys + user + assistant */
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 3 entries, got %d", count);
        entry_branch_free(branch, count);
        db_close(db);
        FAIL(msg);
    }
    if (branch[2].message.role != ROLE_ASSISTANT) {
        entry_branch_free(branch, count);
        db_close(db);
        FAIL("last entry not assistant");
    }
    if (!branch[2].message.content || strcmp(branch[2].message.content, "Hello there!") != 0) {
        entry_branch_free(branch, count);
        db_close(db);
        FAIL("wrong content");
    }
    entry_branch_free(branch, count);
    db_close(db);
    unlink(db_path);
    PASS();
}

/* Test: LLM child returns 10 on tool_calls response */
static void test_llm_child_tool_calls(void) {
    TEST(llm_child_tool_calls);

    const char *db_path = "/tmp/cclaw_llm_child_tc_test.db";
    unlink(db_path);

    mock_server_reset();
    mock_server_enqueue(200, TOOL_CALL_RESPONSE);

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_turn(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "Read foo.txt"};
    entry_append_with_turn(db, sid, &user, 1);
    db_close(db);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "128000", 1);
    setenv("CCLAW_DB", db_path, 1);
    setenv("CCLAW_STREAM", "0", 1);
    setenv("CCLAW_MAX_ITERATIONS", "5", 1);
    setenv("CCLAW_AUTO_RECALL", "0", 1);

    int rc = llm_proc_main(sid);
    if (rc != LLM_EXIT_TOOLCALL) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected exit 10, got %d", rc);
        FAIL(msg);
    }

    /* Verify entry was written with tool_calls */
    db = test_db_open(db_path);
    assert(db);
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    if (count != 4) { /* sys + user + assistant_message + tool_call */
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 4 entries, got %d", count);
        entry_branch_free(branch, count);
        db_close(db);
        FAIL(msg);
    }
    if (branch[2].message.role != ROLE_ASSISTANT) {
        entry_branch_free(branch, count);
        db_close(db);
        FAIL("entry[2] not assistant");
    }
    /* Check tool_calls written — query tool_calls table */
    sqlite3_stmt *stmt;
    int tc_count = 0;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM tool_calls WHERE session_id=?;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, sid);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            tc_count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    entry_branch_free(branch, count);
    db_close(db);
    unlink(db_path);

    if (tc_count != 1) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected 1 tool_call row, got %d", tc_count);
        FAIL(msg);
    }
    PASS();
}

/* Test: LLM child returns 1 on error (server returns 500) */
static void test_llm_child_error(void) {
    TEST(llm_child_error);

    const char *db_path = "/tmp/cclaw_llm_child_err_test.db";
    unlink(db_path);

    mock_server_reset();
    mock_server_enqueue(500, "{\"error\":\"internal\"}");

    sqlite3 *db = test_db_open(db_path);
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append_with_turn(db, sid, &sys, 1);
    Message user = {.role = ROLE_USER, .content = "Hello"};
    entry_append_with_turn(db, sid, &user, 1);
    db_close(db);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_port);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "128000", 1);
    setenv("CCLAW_DB", db_path, 1);
    setenv("CCLAW_STREAM", "0", 1);
    setenv("CCLAW_MAX_ITERATIONS", "5", 1);
    setenv("CCLAW_AUTO_RECALL", "0", 1);

    int rc = llm_proc_main(sid);
    if (rc != LLM_EXIT_ERROR) {
        char msg[64];
        snprintf(msg, sizeof(msg), "expected exit 1, got %d", rc);
        unlink(db_path);
        FAIL(msg);
    }

    unlink(db_path);
    PASS();
}

int main(void) {
    s_port = mock_server_start();
    if (s_port <= 0) {
        fprintf(stderr, "FAIL: could not start mock server\n");
        return 1;
    }

    printf("test_integration_llm_child (T299):\n");
    test_llm_child_stop();
    test_llm_child_tool_calls();
    test_llm_child_error();

    mock_server_stop();
    printf("\n%d/%d tests passed.\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
