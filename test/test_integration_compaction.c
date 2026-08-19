/* Integration test: async session compaction via worker pool */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <curl/curl.h>
#include <pthread.h>

#include "types.h"
#include "db.h"
#include "llm_worker.h"
#include "llm_proc.h"
#include "config.h"
#include "test_util.h"
#include "mock_server.h"

#define TEST_DB "/tmp/test_cclaw_integ_compact.sqlite"

static sqlite3 *setup(void) {
    test_db_clean(TEST_DB);
    sqlite3 *db = test_db_open(TEST_DB);
    if (db) test_seed_agent(db, "default");
    return db;
}

static void teardown(sqlite3 *db) {
    db_close(db);
    test_db_clean(TEST_DB);
}

/* Test: compaction job can be submitted and processed by worker */
static void test_compaction_job_submission(void) {
    sqlite3 *db = setup();
    assert(db);

    /* Initialize worker pool */
    assert(llm_worker_start(TEST_DB, 2) == 0);

    int64_t sid = session_create(db, "compact_test", "default", -1, 0);
    assert(sid > 0);

    /* Fill session with entries to trigger compaction */
    for (int i = 0; i < 20; i++) {
        char buf[128];
        snprintf(buf, sizeof(buf), "entry %d: this is test content to fill session", i);
        Message m = {
            .role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
            .content = buf,
            .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE
        };
        entry_append_with_iteration(db, sid, &m, 1);
    }

    /* Submit compaction job */
    int rc = llm_worker_submit_compact(db, sid, "default");
    assert(rc == 0);

    /* Check job was inserted */
    const char *sql = "SELECT COUNT(*) FROM llm_jobs WHERE session_id=? AND job_type=1;";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    assert(count == 1);

    llm_worker_stop();
    teardown(db);
    printf("  PASS test_compaction_job_submission\n");
}

/* Test: inbox items queued during compacting state are preserved */
static void test_inbox_during_compacting(void) {
    sqlite3 *db = setup();
    assert(db);

    int64_t sid = session_create(db, "inbox_test", "default", -1, 0);
    assert(sid > 0);

    /* Set to compacting state */
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(session_set_state(db, sid, "idle") == 0);
    assert(session_set_state(db, sid, "compacting") == 0);

    /* Insert inbox items while compacting */
    int64_t inbox_id_1 = inbox_insert(db, sid, "test_source", NULL, "message 1");
    assert(inbox_id_1 > 0);

    int64_t inbox_id_2 = inbox_insert(db, sid, "test_source", NULL, "message 2");
    assert(inbox_id_2 > 0);

    /* Verify inbox items exist and are not consumed */
    int inbox_cnt = inbox_count(db, sid);
    assert(inbox_cnt == 2);

    /* Consume items */
    int consumed = inbox_consume_into_entries(db, sid, 100);
    assert(consumed == 2);

    /* Verify entries were created from inbox */
    const char *check_sql = "SELECT COUNT(*) FROM entries WHERE session_id=? AND role=?;";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, check_sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int(stmt, 2, ROLE_USER);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int entry_count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    assert(entry_count == 2);

    teardown(db);
    printf("  PASS test_inbox_during_compacting\n");
}

/* Test: entry_compact correctly summarizes and reparents */
static void test_entry_compact_basic(void) {
    sqlite3 *db = setup();
    assert(db);

    int64_t sid = session_create(db, "compact_basic", "default", -1, 0);
    assert(sid > 0);

    /* Create a simple branch */
    for (int i = 0; i < 5; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "message %d", i);
        Message m = {.role = ROLE_USER, .content = buf};
        entry_append_with_iteration(db, sid, &m, 1);
    }

    /* Get entry IDs */
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    assert(entries != NULL);
    assert(count == 5);

    int64_t last_kept_id = entries[0].id;
    int64_t first_after_id = entries[2].id;

    entry_branch_free(entries, count);

    /* Call entry_compact */
    int64_t compact_id = entry_compact(db, sid, last_kept_id, first_after_id, "Summarized: first 2 messages");
    assert(compact_id > 0);

    /* Verify compaction entry was inserted with correct parent */
    const char *sql = "SELECT parent_id, content, role FROM entries WHERE id=?;";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, compact_id);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int64_t parent = sqlite3_column_int64(stmt, 0);
    int role = sqlite3_column_int(stmt, 2);
    sqlite3_finalize(stmt);

    assert(parent == last_kept_id);
    assert(role == ROLE_COMPACTION);

    /* Verify first_after was reparented */
    assert(sqlite3_prepare_v2(db, "SELECT parent_id FROM entries WHERE id=?;", -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, first_after_id);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int64_t new_parent = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    assert(new_parent == compact_id);

    teardown(db);
    printf("  PASS test_entry_compact_basic\n");
}

/* Drive llm_compaction against the mock server: over-threshold branch gets
 * summarized into a ROLE_COMPACTION entry with the LLM's text */
static void test_llm_compaction_summary(int port) {
    mock_server_reset();
    assert(mock_server_load("test/fixtures/compaction_summary.json") == 1);

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "summary_test", "default", -1, 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "500", 1);
    setenv("CCLAW_CONTEXT_THRESHOLD", "0.1", 1);
    setenv("CCLAW_COMPACTION_TARGET", "0.05", 1);
    setenv("CCLAW_COMPACTION", "1", 1);
    setenv("CCLAW_STREAM", "0", 1);

    char buf[64];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "message number %02d padding text here", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append_with_iteration(db, sid, &m, 1);
    }

    CURL *curl = curl_easy_init();
    assert(llm_compaction(db, curl, sid, "default") == 0);
    curl_easy_cleanup(curl);

    assert(mock_server_request_count() == 1);
    const char *req = mock_server_last_request_body();
    assert(req && strstr(req, "message number") && strstr(req, "Summarize"));

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch);
    int found = 0;
    for (int i = 0; i < count; i++)
        if (branch[i].message.role == ROLE_COMPACTION) {
            found = 1;
            assert(strstr(branch[i].message.content, "Goal: Build a widget"));
            /* Nothing open in this session — no coda section at all. */
            assert(!strstr(branch[i].message.content, COMPACTION_CODA_MARKER));
        }
    assert(found);
    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_llm_compaction_summary\n");
}

/* E2: the compaction entry carries the model's prose AND a mechanical coda of
 * live commitments — and the coda is built after the call, so it never appears
 * in the request the compaction model saw. */
static void test_llm_compaction_coda(int port) {
    mock_server_reset();
    assert(mock_server_load("test/fixtures/compaction_summary.json") == 1);

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "coda_test", "default", -1, 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("OPENROUTER_API_KEY", "test-key", 1);
    setenv("CCLAW_MODEL", "test-model", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "500", 1);
    setenv("CCLAW_CONTEXT_THRESHOLD", "0.1", 1);
    setenv("CCLAW_COMPACTION_TARGET", "0.05", 1);
    setenv("CCLAW_COMPACTION", "1", 1);
    setenv("CCLAW_STREAM", "0", 1);

    /* One pending approval, one approved-unconsumed ticket, one running child. */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO approvals (session_id, tool_name, state) VALUES"
        " (?1,'shell_exec','pending'), (?1,'web_fetch','approved');",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    int64_t kid = session_create(db, "coda_child", "default", sid, 1);
    assert(session_set_state(db, kid, "llm_running") == 0);

    char buf[64];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "message number %02d padding text here", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append_with_iteration(db, sid, &m, 1);
    }

    CURL *curl = curl_easy_init();
    assert(llm_compaction(db, curl, sid, "default") == 0);
    curl_easy_cleanup(curl);

    /* The compaction model never handled the coda. */
    const char *req = mock_server_last_request_body();
    assert(req);
    assert(!strstr(req, COMPACTION_CODA_MARKER));
    assert(!strstr(req, "shell_exec"));
    assert(!strstr(req, "coda_child"));

    /* Exactly one role-4 entry, holding prose then coda. */
    int count = 0, found = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch);
    for (int i = 0; i < count; i++)
        if (branch[i].message.role == ROLE_COMPACTION) {
            const char *c = branch[i].message.content;
            found++;
            const char *prose = strstr(c, "Goal: Build a widget");
            const char *coda = strstr(c, COMPACTION_CODA_MARKER);
            assert(prose && coda && prose < coda);
            assert(strstr(coda, "shell_exec"));
            assert(!strstr(coda, "web_fetch"));  /* approved = decided = absent */
            assert(strstr(coda, "llm_running"));
        }
    assert(found == 1);
    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_llm_compaction_coda\n");
}

/* Under-threshold branch: no HTTP request at all (regression: compaction
 * used to fire on every turn regardless of branch size) */
static void test_llm_compaction_under_threshold(int port) {
    mock_server_reset();
    (void)port;

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "small_test", "default", -1, 0);
    setenv("CCLAW_CONTEXT_WINDOW", "128000", 1);
    setenv("CCLAW_CONTEXT_THRESHOLD", "0.6", 1);

    Message m = {.role = ROLE_USER, .content = "hi", .stop_reason = STOP_REASON_NONE};
    entry_append_with_iteration(db, sid, &m, 1);

    CURL *curl = curl_easy_init();
    assert(llm_compaction(db, curl, sid, "default") == 0);
    curl_easy_cleanup(curl);

    assert(mock_server_request_count() == 0);
    teardown(db);
    printf("  PASS test_llm_compaction_under_threshold\n");
}

/* LLM failure: no compaction entry, branch untouched — and open state does NOT
 * buy a coda-only entry (E2 rides the summary or not at all). */
static void test_llm_compaction_failure_skips(int port) {
    mock_server_reset();
    mock_server_enqueue(500, "{\"error\":\"internal server error\"}");

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "fallback_test", "default", -1, 0);

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO approvals (session_id, tool_name, state)"
        " VALUES (?1,'shell_exec','pending');", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("CCLAW_CONTEXT_WINDOW", "500", 1);
    setenv("CCLAW_CONTEXT_THRESHOLD", "0.1", 1);
    setenv("CCLAW_COMPACTION_TARGET", "0.05", 1);

    char buf[64];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "message number %02d padding text here", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append_with_iteration(db, sid, &m, 1);
    }

    CURL *curl = curl_easy_init();
    assert(llm_compaction(db, curl, sid, "default") == -1);
    curl_easy_cleanup(curl);

    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch);
    for (int i = 0; i < count; i++) {
        assert(branch[i].message.role != ROLE_COMPACTION);
        assert(!strstr(branch[i].message.content, COMPACTION_CODA_MARKER));
    }
    entry_branch_free(branch, count);
    teardown(db);
    printf("  PASS test_llm_compaction_failure_skips\n");
}

/* Gemini endpoint: the request body must be Gemini-shaped
 * (systemInstruction/contents), never OpenAI's messages array.
 *
 * Regression: the body was always built OpenAI-shaped while the URL and auth
 * header followed endpoint_type. Once config_load's key-availability scan
 * promoted a native-Gemini provider (the live production shape — keyless
 * openrouter at priority 0, keyed gemini behind it), every compaction POSTed
 * the wrong shape to :generateContent and 400'd. Silently: the session just
 * kept growing until it overflowed the context window many turns later. */
static void test_llm_compaction_gemini_shape(int port) {
    mock_server_reset();
    mock_server_enqueue(200,
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":"
        "\"Goal: Ship the gemini fix. Next steps: verify shape.\"}]}}]}");

    sqlite3 *db = setup();
    int64_t sid = session_create(db, "gemini_compact", "default", -1, 0);

    char url[64];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1", port);
    setenv("CCLAW_PROVIDER_BASE_URL", url, 1);
    setenv("CCLAW_PROVIDER_ENDPOINT_TYPE", "gemini", 1);
    setenv("CCLAW_MODEL", "gemini-3.5-flash-lite", 1);
    setenv("CCLAW_CONTEXT_WINDOW", "500", 1);
    setenv("CCLAW_CONTEXT_THRESHOLD", "0.1", 1);
    setenv("CCLAW_COMPACTION_TARGET", "0.05", 1);
    setenv("CCLAW_COMPACTION", "1", 1);
    setenv("CCLAW_STREAM", "0", 1);

    char buf[64];
    for (int i = 0; i < 20; i++) {
        snprintf(buf, sizeof(buf), "message number %02d padding text here", i);
        Message m = {.role = (i % 2 == 0) ? ROLE_USER : ROLE_ASSISTANT,
                     .content = buf,
                     .stop_reason = (i % 2 == 1) ? STOP_REASON_STOP : STOP_REASON_NONE};
        entry_append_with_iteration(db, sid, &m, 1);
    }

    CURL *curl = curl_easy_init();
    assert(llm_compaction(db, curl, sid, "default") == 0);
    curl_easy_cleanup(curl);

    assert(mock_server_request_count() == 1);
    const char *req = mock_server_last_request_body();
    assert(req);
    assert(strstr(req, "\"contents\""));          /* Gemini framing */
    assert(strstr(req, "systemInstruction"));
    assert(strstr(req, "maxOutputTokens"));
    assert(!strstr(req, "\"messages\""));         /* ...and NOT OpenAI's */
    assert(!strstr(req, "max_tokens"));
    assert(strstr(req, "message number"));        /* excerpt still carried */
    assert(strstr(req, "Summarize"));             /* system prompt still carried */

    /* Endpoint-aware on both halves: the Gemini response parsed too. */
    int count = 0;
    Entry *branch = session_get_branch(db, sid, &count);
    assert(branch);
    int found = 0;
    for (int i = 0; i < count; i++)
        if (branch[i].message.role == ROLE_COMPACTION) {
            found = 1;
            assert(strstr(branch[i].message.content, "Ship the gemini fix"));
        }
    assert(found);
    entry_branch_free(branch, count);

    /* Must not leak into later tests in this binary. */
    unsetenv("CCLAW_PROVIDER_ENDPOINT_TYPE");
    teardown(db);
    printf("  PASS test_llm_compaction_gemini_shape\n");
}

void run_tests(void) {
}

int main(void) {
    TEST_INIT();
    int port = mock_server_start();
    test_compaction_job_submission();
    test_inbox_during_compacting();
    test_entry_compact_basic();
    test_llm_compaction_under_threshold(port);
    test_llm_compaction_summary(port);
    test_llm_compaction_coda(port);
    test_llm_compaction_gemini_shape(port);
    test_llm_compaction_failure_skips(port);
    mock_server_stop();
    printf("All compaction tests passed\n");
    return 0;
}
