#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "db_response.h"
#include "db.h"
#include "test_util.h"

/* db_ingest_response: parse a response body straight into entries + tool_calls. */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static sqlite3 *fresh_db(int64_t *sid_out) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    sqlite3_exec(db,
        "CREATE TABLE IF NOT EXISTS tool_calls ("
        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  session_id INTEGER NOT NULL, entry_id INTEGER NOT NULL,"
        "  call_id TEXT NOT NULL, name TEXT NOT NULL, arguments TEXT,"
        "  status TEXT NOT NULL DEFAULT 'pending', result_entry_id INTEGER);",
        NULL, NULL, NULL);
    *sid_out = session_create(db, "t", NULL, -1, 0);
    return db;
}

/* Fetch assistant_message content for a session (NULL-safe; caller frees). */
static char *asst_content(sqlite3 *db, int64_t sid) {
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT content FROM entries WHERE session_id=? AND type='assistant_message'"
        " ORDER BY id DESC LIMIT 1;", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, sid);
    char *r = NULL;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(s, 0);
        r = t ? strdup(t) : NULL;
    }
    sqlite3_finalize(s);
    return r;
}

static void test_content_response(void) {
    TEST(content_response);
    int64_t sid; sqlite3 *db = fresh_db(&sid);

    const char *json =
        "{\"choices\":[{\"message\":{\"content\":\"Hello world\",\"role\":\"assistant\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";

    TypedIngestResult ir;
    LlmRespStatus st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, NULL, 1, &ir);
    if (st != LLM_RESP_OK) FAIL("ingest failed");
    if (ir.prompt_tokens != 10 || ir.completion_tokens != 5) FAIL("wrong usage");
    char *c = asst_content(db, sid);
    if (!c || strcmp(c, "Hello world") != 0) { free(c); FAIL("wrong content"); }
    free(c);
    db_close(db);
    PASS();
}

static void test_tool_calls_response(void) {
    TEST(tool_calls_response);
    int64_t sid; sqlite3 *db = fresh_db(&sid);

    const char *json =
        "{\"choices\":[{\"message\":{\"content\":null,\"role\":\"assistant\","
        "\"tool_calls\":[{\"id\":\"call_abc\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell_exec\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":10,\"total_tokens\":30}}";

    TypedIngestResult ir;
    LlmRespStatus st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, NULL, 1, &ir);
    if (st != LLM_RESP_OK) FAIL("ingest failed");

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db,
        "SELECT tc.call_id, tc.name, e.content FROM tool_calls tc"
        " JOIN entries e ON e.id=tc.entry_id WHERE tc.session_id=?;", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, sid);
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_finalize(s); FAIL("no tool_call row"); }
    if (strcmp((const char *)sqlite3_column_text(s, 0), "call_abc") != 0) { sqlite3_finalize(s); FAIL("wrong tc id"); }
    if (strcmp((const char *)sqlite3_column_text(s, 1), "shell_exec") != 0) { sqlite3_finalize(s); FAIL("wrong tc name"); }
    if (strcmp((const char *)sqlite3_column_text(s, 2), "{\"cmd\":\"ls\"}") != 0) { sqlite3_finalize(s); FAIL("wrong tc args"); }
    sqlite3_finalize(s);
    db_close(db);
    PASS();
}

static void test_malformed(void) {
    TEST(malformed);
    int64_t sid; sqlite3 *db = fresh_db(&sid);
    TypedIngestResult ir;
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, "not json", NULL, 1, &ir) != LLM_RESP_MALFORMED)
        FAIL("should fail on invalid JSON");
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, NULL, NULL, 1, &ir) != LLM_RESP_MALFORMED)
        FAIL("should fail on NULL");
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, "{\"choices\":[]}", NULL, 1, &ir) != LLM_RESP_MALFORMED)
        FAIL("should fail on empty choices");
    db_close(db);
    PASS();
}

static void test_cost_field(void) {
    TEST(cost_field);
    int64_t sid; sqlite3 *db = fresh_db(&sid);
    const char *json =
        "{\"choices\":[{\"message\":{\"content\":\"hi\",\"role\":\"assistant\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":5,\"total_tokens\":105,"
        "\"cost\":0.000072112}}";

    TypedIngestResult ir;
    LlmRespStatus st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, NULL, 1, &ir);
    if (st != LLM_RESP_OK) FAIL("ingest failed");
    if (ir.cost_nano != 72112) {
        char buf[64]; snprintf(buf, sizeof(buf), "expected 72112, got %lld", (long long)ir.cost_nano);
        FAIL(buf);
    }
    db_close(db);
    PASS();
}

/* Bug 9: $.provider (the upstream backend) is a different field from $.id
 * (the request id) — both must land, each in its own column. */
static void test_upstream_provider_archived(void) {
    TEST(upstream_provider_archived);
    int64_t sid; sqlite3 *db = fresh_db(&sid);
    const char *json =
        "{\"id\":\"gen-1787171421-abc\",\"provider\":\"StreamLake\","
        "\"choices\":[{\"message\":{\"content\":\"hi\",\"role\":\"assistant\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}";
    TypedIngestResult ir;
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, NULL, 1, &ir) != LLM_RESP_OK)
        FAIL("ingest failed");

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT provider_id, upstream_provider FROM llm_responses"
                           " ORDER BY id DESC LIMIT 1", -1, &s, NULL);
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_finalize(s); FAIL("no archive row"); }
    const char *pid = (const char *)sqlite3_column_text(s, 0);
    const char *up  = (const char *)sqlite3_column_text(s, 1);
    int ok = pid && up && strcmp(pid, "gen-1787171421-abc") == 0 &&
             strcmp(up, "StreamLake") == 0;
    sqlite3_finalize(s);
    if (!ok) FAIL("provider_id/upstream_provider wrong");

    /* No $.provider (most providers) → NULL, not an empty string. */
    const char *plain =
        "{\"choices\":[{\"message\":{\"content\":\"hi\",\"role\":\"assistant\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}";
    if (db_ingest_response(db, sid, 2, "m", ENDPOINT_OPENAI, plain, NULL, 1, &ir) != LLM_RESP_OK)
        FAIL("second ingest failed");
    sqlite3_prepare_v2(db, "SELECT upstream_provider IS NULL FROM llm_responses"
                           " ORDER BY id DESC LIMIT 1", -1, &s, NULL);
    ok = (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);
    if (!ok) FAIL("absent $.provider should be NULL");
    db_close(db);
    PASS();
}

/* Bug 4: the tool call came back as raw DSML markup inside `reasoning`, with
 * content null and no tool_calls — retryable, never a silent empty turn. */
static void test_dsml_in_reasoning_classified(void) {
    TEST(dsml_in_reasoning_classified);
    int64_t sid; sqlite3 *db = fresh_db(&sid);
    const char *json =
        "{\"provider\":\"StreamLake\","
        "\"choices\":[{\"message\":{\"content\":null,\"role\":\"assistant\","
        "\"reasoning\":\"I should run this. <｜DSML｜tool_calls>"
        "<｜DSML｜invoke name=js_eval>1+1\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":367,\"total_tokens\":467}}";
    TypedIngestResult ir;
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, NULL, 1, &ir) != LLM_RESP_TOOL_MARKUP)
        FAIL("DSML-in-reasoning should classify as tool markup");

    /* No entries written — the turn must not absorb a half-response. */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM entries WHERE session_id=?", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, sid);
    int n = (sqlite3_step(s) == SQLITE_ROW) ? sqlite3_column_int(s, 0) : -1;
    sqlite3_finalize(s);
    if (n != 0) FAIL("no entries expected");

    /* Archived under its own status, with the upstream that did it. */
    sqlite3_prepare_v2(db, "SELECT status, upstream_provider FROM llm_responses"
                           " ORDER BY id DESC LIMIT 1", -1, &s, NULL);
    int ok = 0;
    if (sqlite3_step(s) == SQLITE_ROW) {
        const char *st_ = (const char *)sqlite3_column_text(s, 0);
        const char *up  = (const char *)sqlite3_column_text(s, 1);
        ok = st_ && up && strcmp(st_, "tool_markup") == 0 && strcmp(up, "StreamLake") == 0;
    }
    sqlite3_finalize(s);
    if (!ok) FAIL("archive row wrong");

    /* The ASCII-marker variant of the same failure. */
    int64_t sid2; sqlite3 *db2 = fresh_db(&sid2);
    const char *ascii =
        "{\"choices\":[{\"message\":{\"content\":\"\",\"role\":\"assistant\","
        "\"reasoning\":\"plan: <|tool_call|>{\\\"name\\\":\\\"js_eval\\\"}\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":40,\"total_tokens\":49}}";
    LlmRespStatus st2 = db_ingest_response(db2, sid2, 1, "m", ENDPOINT_OPENAI, ascii, NULL, 1, &ir);
    db_close(db2);
    if (st2 != LLM_RESP_TOOL_MARKUP) FAIL("ASCII <| marker should classify too");

    db_close(db);
    PASS();
}

/* The negative half: ordinary reasoning — including reasoning that merely
 * *talks* about tool calls, or contains stray angle brackets — must ingest
 * normally. Markup alongside a real tool call is not the failure either. */
static void test_reasoning_without_markup_is_normal(void) {
    TEST(reasoning_without_markup_is_normal);
    int64_t sid; sqlite3 *db = fresh_db(&sid);
    TypedIngestResult ir;

    const char *prose =
        "{\"choices\":[{\"message\":{\"content\":\"Two.\",\"role\":\"assistant\","
        "\"reasoning\":\"I could use the js_eval tool_call here, or invoke name lookup, "
        "but 1+1 is 2. Comparing a<b and b>c too.\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":9,\"total_tokens\":14}}";
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, prose, NULL, 1, &ir) != LLM_RESP_OK)
        FAIL("prose reasoning must not classify");

    const char *empty_reason =
        "{\"choices\":[{\"message\":{\"content\":null,\"role\":\"assistant\","
        "\"reasoning\":\"thinking about <html> tags and 3 < 4\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5}}";
    if (db_ingest_response(db, sid, 2, "m", ENDPOINT_OPENAI, empty_reason, NULL, 1, &ir) != LLM_RESP_OK)
        FAIL("markup-free empty content must not classify");

    const char *with_calls =
        "{\"choices\":[{\"message\":{\"content\":null,\"role\":\"assistant\","
        "\"reasoning\":\"<｜DSML｜tool_calls>\","
        "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"js_eval\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4,\"total_tokens\":7}}";
    if (db_ingest_response(db, sid, 3, "m", ENDPOINT_OPENAI, with_calls, NULL, 1, &ir) != LLM_RESP_OK)
        FAIL("a real tool call must win over stray markup");
    db_close(db);
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("--- test_llm ---\n");
    test_content_response();
    test_tool_calls_response();
    test_malformed();
    test_cost_field();
    test_upstream_provider_archived();
    test_dsml_in_reasoning_classified();
    test_reasoning_without_markup_is_normal();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
