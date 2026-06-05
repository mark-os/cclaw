/* T300: Integration test for parent turn loop (fork LLM child → dispatch tools → loop) */
#define _POSIX_C_SOURCE 200809L
#include "agent_turn.h"
#include "agent_exit.h"
#include "config.h"
#include "db.h"
#include "mock_server.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_parent_turn/cclaw.db"
#define WORK_DIR "/tmp/test_parent_turn"

static int s_mock_port;

static void cleanup(void) { system("rm -rf " WORK_DIR); }

static sqlite3 *setup_db(void) {
    cleanup();
    system("mkdir -p " WORK_DIR "/agents/default/workspace");
    sqlite3 *db = db_open_cclaw(DB_PATH);
    assert(db);
    return db;
}

/* Test: simple stop response — LLM child returns stop, parent exits DONE */
static void test_simple_stop(void) {
    sqlite3 *db = setup_db();

    int64_t sid = session_create(db, "test", "default", -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "hello"};
    entry_append(db, sid, &user_msg);
    inbox_insert(db, sid, "cli", "hello");

    /* Enqueue mock response: plain text, no tool_calls */
    mock_server_enqueue(200,
        "{\"id\":\"x\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"Hi there!\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":3,\"total_tokens\":8}}");

    db_close(db);

    /* Configure env */
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_mock_port);
    setenv("CCLAW_PROVIDER", url, 1);
    setenv("CCLAW_API_KEY", "test", 1);
    setenv("CCLAW_MODEL", "test", 1);
    setenv("CCLAW_AGENT_DB", DB_PATH, 1);
    setenv("CCLAW_AGENT_NAME", "default", 1);
    setenv("CCLAW_MODE", "cli", 1);
    setenv("CCLAW_STREAM", "0", 1);
    setenv("CCLAW_MAX_TOKENS", "1000", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "4096", 1);
    setenv("CCLAW_LOG_LEVEL", "debug", 1);
    setenv("CCLAW_TOOLS", "file_read", 1);
    unsetenv("CCLAW_LLM_MOCK");

    char sid_str[32];
    snprintf(sid_str, sizeof(sid_str), "%lld", (long long)sid);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        _exit(agent_turn_run(sid));
    }
    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == AGENT_EXIT_DONE);

    /* Verify response in DB */
    db = db_open_cclaw(DB_PATH);
    assert(db);
    char *resp = get_response_text(db, sid);
    assert(resp && strstr(resp, "Hi there!"));
    free(resp);
    db_close(db);
    cleanup();
    printf("  PASS test_simple_stop\n");
}

/* Test: tool_call → dispatch → loop → stop */
static void test_tool_dispatch(void) {
    sqlite3 *db = setup_db();

    int64_t sid = session_create(db, "test", "default", -1, 0);
    Message user_msg = {.role = ROLE_USER, .content = "read test.txt"};
    entry_append(db, sid, &user_msg);

    /* Write a file for file_read to find */
    FILE *f = fopen(WORK_DIR "/agents/default/workspace/test.txt", "w");
    assert(f);
    fprintf(f, "file content here");
    fclose(f);

    /* First response: tool_call for file_read */
    mock_server_enqueue(200,
        "{\"id\":\"1\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":null,"
        "\"tool_calls\":[{\"id\":\"call_1\",\"type\":\"function\","
        "\"function\":{\"name\":\"file_read\",\"arguments\":\"{\\\"path\\\":\\\"test.txt\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}");

    /* Second response: final text after tool result */
    mock_server_enqueue(200,
        "{\"id\":\"2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"The file contains: file content here\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":8,\"total_tokens\":28}}");

    db_close(db);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", s_mock_port);
    setenv("CCLAW_PROVIDER", url, 1);
    setenv("CCLAW_API_KEY", "test", 1);
    setenv("CCLAW_MODEL", "test", 1);
    setenv("CCLAW_AGENT_DB", DB_PATH, 1);
    setenv("CCLAW_AGENT_NAME", "default", 1);
    setenv("CCLAW_MODE", "cli", 1);
    setenv("CCLAW_STREAM", "0", 1);
    setenv("CCLAW_MAX_TOKENS", "1000", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "4096", 1);
    setenv("CCLAW_LOG_LEVEL", "debug", 1);
    setenv("CCLAW_TOOLS", "file_read", 1);
    setenv("CCLAW_WORKSPACE", WORK_DIR "/agents/default/workspace", 1);
    unsetenv("CCLAW_LLM_MOCK");

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        _exit(agent_turn_run(sid));
    }
    int status;
    waitpid(pid, &status, 0);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == AGENT_EXIT_DONE);

    /* Verify final response */
    db = db_open_cclaw(DB_PATH);
    assert(db);
    char *resp = get_response_text(db, sid);
    assert(resp && strstr(resp, "file content here"));
    free(resp);

    /* Verify tool_calls table has a completed entry */
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db,
        "SELECT status FROM tool_calls WHERE session_id=? AND name='file_read';",
        -1, &stmt, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    const char *tc_status = (const char *)sqlite3_column_text(stmt, 0);
    assert(tc_status && strcmp(tc_status, "done") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    cleanup();
    printf("  PASS test_tool_dispatch\n");
}

int main(void) {
    printf("test_integration_parent_turn (T300):\n");
    s_mock_port = mock_server_start();
    test_simple_stop();
    test_tool_dispatch();
    mock_server_stop();
    printf("All parent turn loop tests passed.\n");
    return 0;
}
