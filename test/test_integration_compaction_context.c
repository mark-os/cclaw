/* T185: integration test — compaction context.
 * Insert 300 entries, trigger compaction, run agent loop w/ mock server
 * post-compaction; verify CTE returns summary + recent only; verify FTS5
 * still indexes old entries. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <curl/curl.h>
#include "agent.h"
#include "context.h"
#include "db.h"
#include "mock_server.h"

#define TEST_DB "/tmp/test_cclaw_integ_compact_ctx.sqlite"

static sqlite3 *setup(void) {
    unlink(TEST_DB);
    return db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    unlink(TEST_DB);
}

static char *mock_dispatch(const char *name, const char *arguments, void *user_data) {
    (void)name; (void)arguments; (void)user_data;
    return strdup("ok");
}

static const char *MOCK_SUMMARY =
    "{\"id\":\"c\",\"choices\":[{\"message\":{\"role\":\"assistant\","
    "\"content\":\"Goal: Testing. Progress: 300 messages exchanged. "
    "Decisions: None. Next: Continue.\"},\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":20,\"total_tokens\":120}}";

static const char *MOCK_FINAL =
    "{\"id\":\"c2\",\"choices\":[{\"message\":{\"role\":\"assistant\","
    "\"content\":\"Done after compaction.\"},\"finish_reason\":\"stop\"}],"
    "\"usage\":{\"prompt_tokens\":50,\"completion_tokens\":5,\"total_tokens\":55}}";

/* Insert N alternating user/assistant entries */
static void seed_entries(sqlite3 *db, int64_t sid, int n) {
    char buf[128];
    for (int i = 0; i < n; i++) {
        snprintf(buf, sizeof(buf), "entry_content_%04d unique_word_%04d", i, i);
        Message m = {
            .role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
            .content = buf,
            .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE
        };
        entry_append(db, sid, &m);
    }
}

/* Test: compaction + agent loop post-compaction */
static void test_compaction_then_agent(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    int port = mock_server_start();
    assert(port > 0);

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "compact_integ", NULL, -1, 0);
    assert(sid > 0);

    /* Insert 300 entries */
    seed_entries(db, sid, 300);

    /* Config: small budget to force compaction */
    char base_url[64];
    snprintf(base_url, sizeof(base_url), "http://127.0.0.1:%d/v1", port);

    Config cfg = {0};
    cfg.max_history_tokens = 200;       /* tiny — forces most entries out */
    cfg.compaction_threshold = 5;
    cfg.provider.context_window = 4096;
    cfg.provider.base_url = base_url;
    cfg.provider.api_key = "test-key";
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.max_iterations = 10;
    cfg.system_prompt = "You are a test assistant.";

    /* Verify compaction is needed */
    assert(session_needs_compaction(db, sid, &cfg) == 1);

    /* Enqueue mock response for compaction LLM call */
    mock_server_enqueue(200, MOCK_SUMMARY);

    /* Trigger compaction */
    int64_t cid = session_try_compact(db, sid, &cfg);
    assert(cid > 0);

    /* Verify CTE branch: should have root + compaction + recent entries (much less than 300) */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    assert(count < 300);
    assert(count >= 3); /* at least root + compaction + one recent */

    /* Verify compaction entry present */
    int found_compaction = 0;
    for (int i = 0; i < count; i++) {
        if (branch[i].message.role == ROLE_COMPACTION) {
            found_compaction = 1;
            assert(strstr(branch[i].message.content, "Goal: Testing") != NULL);
            break;
        }
    }
    assert(found_compaction);
    entry_branch_free(branch, count);

    /* Now add a user message and run agent loop post-compaction */
    Message user_msg = {.role = ROLE_USER, .content = "Continue after compaction"};
    entry_append(db, sid, &user_msg);

    /* Enqueue mock final response for agent loop */
    mock_server_enqueue(200, MOCK_FINAL);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = mock_dispatch;
    ctx.tools = NULL;
    ctx.tool_count = 0;

    int rc = agent_run(&ctx);
    assert(rc == 0);

    /* Verify agent produced a response */
    branch = session_get_branch(db, sid, &count);
    assert(branch != NULL);
    /* Last entry should be assistant final */
    assert(branch[count - 1].message.role == ROLE_ASSISTANT);
    assert(strstr(branch[count - 1].message.content, "Done after compaction") != NULL);
    entry_branch_free(branch, count);

    /* Verify FTS5 still indexes old (compacted) entries */
    int search_count = 0;
    Entry *results = entry_search(db, "unique_word_0050", sid, &search_count);
    assert(search_count >= 1);
    entry_branch_free(results, search_count);

    /* Also search for a word from the very first entry */
    results = entry_search(db, "unique_word_0000", sid, &search_count);
    assert(search_count >= 1);
    entry_branch_free(results, search_count);

    /* Verify LLM was called: 1 for compaction summary + 1 for agent loop */
    assert(mock_server_request_count() == 2);

    teardown(db);
    mock_server_stop();
    curl_global_cleanup();
    printf("  PASS test_compaction_then_agent\n");
}

/* Test: FTS5 indexes old entries after compaction (dedicated check) */
static void test_fts5_after_compaction(void) {
    sqlite3 *db = setup();
    int64_t sid = session_create(db, "fts_compact", NULL, -1, 0);
    assert(sid > 0);

    seed_entries(db, sid, 300);

    Config cfg = {0};
    cfg.max_history_tokens = 200;
    cfg.compaction_threshold = 5;
    cfg.provider.context_window = 4096;
    /* No provider — uses placeholder summary */

    int64_t cid = session_try_compact(db, sid, &cfg);
    assert(cid > 0);

    /* Search for entries scattered throughout the compacted range */
    int search_count = 0;
    for (int i = 10; i < 250; i += 50) {
        char query[32];
        snprintf(query, sizeof(query), "unique_word_%04d", i);
        Entry *results = entry_search(db, query, sid, &search_count);
        assert(search_count >= 1);
        entry_branch_free(results, search_count);
    }

    teardown(db);
    printf("  PASS test_fts5_after_compaction\n");
}

int main(void) {
    printf("--- test_integration_compaction_context (T185) ---\n");
    test_compaction_then_agent();
    test_fts5_after_compaction();
    printf("All compaction context integration tests passed.\n");
    return 0;
}
