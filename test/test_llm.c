#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "db_response.h"
#include "db.h"
#include "test_util.h"

/* db_ingest_response: what an OpenAI-compatible body says about who served it
 * and whether a tool call leaked into the reasoning as markup. */

static sqlite3 *fresh_db(int64_t *sid_out) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    *sid_out = session_create(db, "t", NULL, -1, 0);
    return db;
}

/* Bug 9: $.provider (the upstream backend) is a different field from $.id
 * (the request id): both must land, each in its own column. */
static void test_upstream_provider_archived(void) {
    int64_t sid; sqlite3 *db = fresh_db(&sid);
    const char *json =
        "{\"id\":\"gen-1787171421-abc\",\"provider\":\"StreamLake\","
        "\"choices\":[{\"message\":{\"content\":\"hi\",\"role\":\"assistant\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}";
    TypedIngestResult ir;
    assert(db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, json, NULL, 1, &ir) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id=1"
                       " AND provider_id='gen-1787171421-abc'"
                       " AND upstream_provider='StreamLake'") == 1);

    /* No $.provider (most providers): NULL, not an empty string. */
    const char *plain =
        "{\"choices\":[{\"message\":{\"content\":\"hi\",\"role\":\"assistant\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1,\"total_tokens\":2}}";
    assert(db_ingest_response(db, sid, 2, "m", ENDPOINT_OPENAI, plain, NULL, 1, &ir) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id=2"
                       " AND upstream_provider IS NULL") == 1);
    db_close(db);
}

/* Bug 4: the tool call came back as raw DSML markup inside `reasoning`, with
 * content null and no tool_calls: retryable, never a silent empty turn. The
 * ASCII-marker variant is the same failure. Ordinary reasoning, including
 * reasoning that merely *talks* about tool calls or holds stray angle
 * brackets, ingests normally, and markup beside a real tool call is not the
 * failure either. */
static void test_tool_markup_in_reasoning(void) {
    static const struct { const char *body; LlmRespStatus want; } cases[] = {
        { "{\"provider\":\"StreamLake\","
          "\"choices\":[{\"message\":{\"content\":null,\"role\":\"assistant\","
          "\"reasoning\":\"I should run this. <｜DSML｜tool_calls>"
          "<｜DSML｜invoke name=js_eval>1+1\"},"
          "\"finish_reason\":\"stop\"}],"
          "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":367,\"total_tokens\":467}}",
          LLM_RESP_TOOL_MARKUP },
        { "{\"choices\":[{\"message\":{\"content\":\"\",\"role\":\"assistant\","
          "\"reasoning\":\"plan: <|tool_call|>{\\\"name\\\":\\\"js_eval\\\"}\"},"
          "\"finish_reason\":\"stop\"}],"
          "\"usage\":{\"prompt_tokens\":9,\"completion_tokens\":40,\"total_tokens\":49}}",
          LLM_RESP_TOOL_MARKUP },
        { "{\"choices\":[{\"message\":{\"content\":\"Two.\",\"role\":\"assistant\","
          "\"reasoning\":\"I could use the js_eval tool_call here, or invoke name lookup, "
          "but 1+1 is 2. Comparing a<b and b>c too.\"},"
          "\"finish_reason\":\"stop\"}],"
          "\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":9,\"total_tokens\":14}}",
          LLM_RESP_OK },
        { "{\"choices\":[{\"message\":{\"content\":null,\"role\":\"assistant\","
          "\"reasoning\":\"thinking about <html> tags and 3 < 4\"},"
          "\"finish_reason\":\"stop\"}],"
          "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":2,\"total_tokens\":5}}",
          LLM_RESP_OK },
        { "{\"choices\":[{\"message\":{\"content\":null,\"role\":\"assistant\","
          "\"reasoning\":\"<｜DSML｜tool_calls>\","
          "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
          "\"function\":{\"name\":\"js_eval\",\"arguments\":\"{}\"}}]},"
          "\"finish_reason\":\"tool_calls\"}],"
          "\"usage\":{\"prompt_tokens\":3,\"completion_tokens\":4,\"total_tokens\":7}}",
          LLM_RESP_OK },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int64_t sid; sqlite3 *db = fresh_db(&sid);
        TypedIngestResult ir;
        LlmRespStatus st = db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI,
                                              cases[i].body, NULL, 1, &ir);
        if (st != cases[i].want) {
            fprintf(stderr, "FAIL cases[%zu]: status %d, want %d\n", i, (int)st, (int)cases[i].want);
            abort();
        }
        if (i == 0) {
            /* No entries: the turn must not absorb a half-response. Archived
             * under its own status, with the upstream that did it. */
            assert(test_qi(db, "SELECT COUNT(*) FROM entries") == 0);
            assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE status='tool_markup'"
                               " AND upstream_provider='StreamLake'") == 1);
        }
        db_close(db);
    }
}

int main(void) {
    TEST_INIT();
    printf("--- test_llm ---\n");
    RUN(test_upstream_provider_archived);
    RUN(test_tool_markup_in_reasoning);
    return 0;
}
