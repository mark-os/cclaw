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
    LlmRespStatus st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, &ir);
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
    LlmRespStatus st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, &ir);
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
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, "not json", &ir) != LLM_RESP_MALFORMED)
        FAIL("should fail on invalid JSON");
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, NULL, &ir) != LLM_RESP_MALFORMED)
        FAIL("should fail on NULL");
    if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, "{\"choices\":[]}", &ir) != LLM_RESP_MALFORMED)
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
    LlmRespStatus st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, &ir);
    if (st != LLM_RESP_OK) FAIL("ingest failed");
    if (ir.cost_nano != 72112) {
        char buf[64]; snprintf(buf, sizeof(buf), "expected 72112, got %lld", (long long)ir.cost_nano);
        FAIL(buf);
    }
    db_close(db);
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("--- test_llm ---\n");
    test_content_response();
    test_tool_calls_response();
    test_malformed();
    test_cost_field();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
