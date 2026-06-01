/* Integration test: compaction summarization via LLM (mock server + fixture). */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <signal.h>
#include <unistd.h>
#include <curl/curl.h>
#include "context.h"
#include "db.h"
#include "mock_server.h"

#define TEST_DB "/tmp/test_cclaw_integ_compact_summary.sqlite"

static sqlite3 *setup(void) {
    unlink(TEST_DB);
    return db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    unlink(TEST_DB);
}

static int s_port;

static void test_llm_summary(void) {
    mock_server_reset();
    int loaded = mock_server_load("test/fixtures/compaction_summary.json");
    assert(loaded == 1);

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "summary_test", NULL, -1, 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.context_threshold = 0.1f;
    cfg.compaction_target = 0.05f;
    cfg.compaction = 1;
    cfg.provider.context_window = 500;
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;

    /* Insert 20 entries to exceed threshold */
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
    assert(mock_server_request_count() == 1);

    /* Verify request contains entry content */
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
    printf("  PASS test_llm_summary\n");
}

static void test_fallback_on_failure(void) {
    mock_server_reset();
    mock_server_enqueue(500, "{\"error\":\"internal server error\"}");

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "fallback_test", NULL, -1, 0);

    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", s_port);

    Config cfg = {0};
    cfg.context_threshold = 0.1f;
    cfg.compaction_target = 0.05f;
    cfg.compaction = 1;
    cfg.provider.context_window = 500;
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";

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
    printf("  PASS test_fallback_on_failure\n");
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    s_port = mock_server_start();
    assert(s_port > 0);

    printf("test_integration_compaction_summary:\n");
    test_llm_summary();
    test_fallback_on_failure();
    printf("All compaction summary integration tests passed.\n");

    mock_server_stop();
    curl_global_cleanup();
    return 0;
}
