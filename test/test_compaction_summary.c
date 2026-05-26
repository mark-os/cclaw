/* T162: test compaction summarization via LLM call.
 * Uses mock server to verify LLM is called and summary content is stored. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <curl/curl.h>
#include "context.h"
#include "db.h"
#include "mock_server.h"

#define TEST_DB "/tmp/test_cclaw_compact_summary.sqlite"

static sqlite3 *setup(void) {
    unlink(TEST_DB);
    return db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    unlink(TEST_DB);
}

/* Mock LLM response with summary content */
static const char *MOCK_SUMMARY_RESPONSE =
    "{\"id\":\"chatcmpl-mock\",\"choices\":[{\"message\":{\"role\":\"assistant\","
    "\"content\":\"Goal: Build a widget. Progress: Completed steps 1-3. "
    "Decisions: Used approach A. Next steps: Implement step 4.\"},"
    "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":100,"
    "\"completion_tokens\":30,\"total_tokens\":130}}";

/* T162: compaction calls LLM and uses response as summary */
static void test_llm_summary(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int port = mock_server_start();
    assert(port > 0);

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "summary_test", NULL, -1, 0);

    /* Config with mock server as provider */
    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    Config cfg = {0};
    cfg.max_history_tokens = 100;
    cfg.compaction_threshold = 5;
    cfg.provider.context_window = 4096;
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;

    /* Insert 20 entries to trigger compaction */
    char buf[64];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "message number %02d padding text here", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append(db, sid, &m);
    }

    /* Enqueue mock LLM response */
    mock_server_enqueue(200, MOCK_SUMMARY_RESPONSE);

    /* Trigger compaction */
    int64_t cid = session_try_compact(db, sid, &cfg);
    assert(cid > 0);

    /* Verify LLM was called */
    assert(mock_server_request_count() == 1);

    /* Verify the request body contains entry content */
    const char *req = mock_server_last_request_body();
    assert(req != NULL);
    assert(strstr(req, "message number") != NULL);
    assert(strstr(req, "Summarize") != NULL);

    /* Verify compaction entry has LLM-generated summary */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_COMPACTION) {
            found = 1;
            assert(strstr(branch[i].message.content, "Goal: Build a widget") != NULL);
            break;
        }
    }
    assert(found);

    entry_branch_free(branch, count);
    teardown(db);
    mock_server_stop();
    curl_global_cleanup();
    printf("  PASS test_llm_summary\n");
}

/* T162: fallback to placeholder when LLM call fails */
static void test_fallback_on_failure(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int port = mock_server_start();
    assert(port > 0);

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "fallback_test", NULL, -1, 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    Config cfg = {0};
    cfg.max_history_tokens = 100;
    cfg.compaction_threshold = 5;
    cfg.provider.context_window = 4096;
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";

    /* Insert 20 entries */
    char buf[64];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "message number %02d padding text here", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append(db, sid, &m);
    }

    /* Enqueue 500 error response */
    mock_server_enqueue(500, "{\"error\":\"internal server error\"}");

    /* Trigger compaction — should fall back to placeholder */
    int64_t cid = session_try_compact(db, sid, &cfg);
    assert(cid > 0);

    /* Verify compaction entry has fallback text */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_COMPACTION) {
            found = 1;
            assert(strstr(branch[i].message.content, "Compacted") != NULL);
            break;
        }
    }
    assert(found);

    entry_branch_free(branch, count);
    teardown(db);
    mock_server_stop();
    curl_global_cleanup();
    printf("  PASS test_fallback_on_failure\n");
}

/* T162: fallback when no provider configured */
static void test_fallback_no_provider(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "no_provider", NULL, -1, 0);

    Config cfg = {0};
    cfg.max_history_tokens = 100;
    cfg.compaction_threshold = 5;
    cfg.provider.context_window = 4096;
    /* No api_key/base_url/model → should use placeholder */

    char buf[64];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "message number %02d padding text here", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append(db, sid, &m);
    }

    int64_t cid = session_try_compact(db, sid, &cfg);
    assert(cid > 0);

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    int found = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_COMPACTION) {
            found = 1;
            assert(strstr(branch[i].message.content, "Compacted") != NULL);
            break;
        }
    }
    assert(found);

    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_fallback_no_provider\n");
}

int main(void) {
    printf("test_compaction_summary:\n");
    test_llm_summary();
    test_fallback_on_failure();
    test_fallback_no_provider();
    printf("All compaction summary tests passed.\n");
    return 0;
}
