#define _POSIX_C_SOURCE 200809L
#include "db_response.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *test_db(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    return db;
}

/* The newest reasoning entry's meta at path equals want. */
static int meta_is(sqlite3 *db, const char *path, const char *want) {
    char *sql = sqlite3_mprintf("SELECT json_extract(reasoning_meta,%Q) FROM entries"
                                " WHERE type='reasoning' ORDER BY id DESC LIMIT 1", path);
    char *v = test_qs(db, sql);
    sqlite3_free(sql);
    int ok = v && strcmp(v, want) == 0;
    if (!ok) fprintf(stderr, "meta %s: got %s, want %s\n", path, v ? v : "(null)", want);
    free(v);
    return ok;
}

/* db_ingest_response writes entries + tool_calls from an OpenAI body */
static void test_ingest_response(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message msg = {.role = ROLE_USER, .content = "do it"};
    entry_append_with_iteration(db, sid, &msg, 1);

    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"Let me list files.\","
        "\"tool_calls\":[{\"id\":\"call_typed\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell_exec\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":50,\"cost\":0.0000015}}";

    TypedIngestResult result;
    LlmRespStatus st = db_ingest_response(db, sid, 1, "deepseek-v4", ENDPOINT_OPENAI,
                                          body, NULL, 1, &result);
    assert(st == LLM_RESP_OK);
    assert(result.assistant_entry_id > 0);
    assert(result.prompt_tokens == 100);
    assert(result.completion_tokens == 50);
    assert(result.cost_nano == 1500);

    /* tool_calls holds workflow state only; the args live in entries.content */
    assert(test_qi(db, "SELECT COUNT(*) FROM tool_calls WHERE call_id='call_typed'"
                       " AND name='shell_exec'") == 1);

    db_close(db);
}

/* malformed body (no choices) returns LLM_RESP_MALFORMED, writes nothing */
static void test_ingest_malformed(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    static const char *bodies[] = { "{\"error\":\"nope\"}", "not json", "{\"choices\":[]}", NULL };
    for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
        TypedIngestResult result;
        if (db_ingest_response(db, sid, 1, "m", ENDPOINT_OPENAI, bodies[i], NULL, 1, &result)
            != LLM_RESP_MALFORMED) {
            fprintf(stderr, "FAIL bodies[%zu] not malformed\n", i);
            abort();
        }
    }
    assert(test_qi(db, "SELECT COUNT(*) FROM entries") == 0);
    db_close(db);
}

/* db_ingest_response archives the raw body to llm_responses as JSONB */
static void test_ingest_archive(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *body =
        "{\"id\":\"resp_abc\",\"choices\":[{\"message\":{\"content\":\"hi\"},"
        "\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";
    TypedIngestResult ir;
    assert(db_ingest_response(db, sid, 7, "m", ENDPOINT_OPENAI, body, NULL, 1, &ir) == LLM_RESP_OK);

    /* Archived as JSONB that stays re-queryable, with the provider's id. */
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id=7"
                       " AND status='ok' AND provider_id='resp_abc' AND typeof(body)='blob'"
                       " AND json_extract(body,'$.choices[0].message.content')='hi'") == 1);
    /* Not-JSON body archives as status='malformed' with the raw text. */
    assert(db_ingest_response(db, sid, 8, "m", ENDPOINT_OPENAI, "<html>nope", NULL, 1, &ir) == LLM_RESP_MALFORMED);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id=8"
                       " AND status='malformed' AND typeof(body)='text' AND body='<html>nope'") == 1);

    db_close(db);
}

/* db_archive_response (any HTTP outcome) + configurable retention */
static void test_archive_retention(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* An error body archives under its label; JSON gets provider_id extracted.
     * The request body we sent is archived alongside (recoverable for debugging). */
    db_archive_response(db, sid, 1, "m", "http_500",
                        "{\"id\":\"err_1\",\"error\":{\"message\":\"boom\"}}",
                        "{\"model\":\"m\",\"messages\":[{\"role\":\"user\"}]}");
    db_archive_response(db, sid, 2, "m", "timeout", "upstream timed out", NULL);  /* not JSON */

    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id=1"
                       " AND status='http_500' AND provider_id='err_1' AND typeof(body)='blob'"
                       " AND json_extract(request_body,'$.model')='m'") == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id=2"
                       " AND status='timeout' AND typeof(body)='text'") == 1);

    /* Cap = 2, per status class: a stream of 'ok' rows prunes older 'ok' rows
     * but never pushes out the failures that error entries cite ("[resp #N]").
     * The two failures above (http_500, timeout) must both survive. */
    sqlite3_exec(db, "INSERT OR REPLACE INTO config(key,value) VALUES('llm_response_archive_max','2');",
                 NULL, NULL, NULL);
    for (int i = 0; i < 5; i++)
        db_archive_response(db, sid, 100 + i, "m", "ok", "{\"x\":1}", NULL);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE status='ok';") == 2);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE status!='ok';") == 2);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id IN (1,2);") == 2);

    /* Failures prune within their own class too: two more push out the first two. */
    db_archive_response(db, sid, 110, "m", "http_502", "oops", NULL);
    db_archive_response(db, sid, 111, "m", "network_error", NULL, NULL);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE status!='ok';") == 2);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id IN (1,2);") == 0);

    /* Cap = -1: pruning disabled, rows accumulate. */
    sqlite3_exec(db, "UPDATE config SET value='-1' WHERE key='llm_response_archive_max';",
                 NULL, NULL, NULL);
    for (int i = 0; i < 5; i++)
        db_archive_response(db, sid, 200 + i, "m", "ok", "{\"x\":1}", NULL);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses;") == 9);

    /* Cap = 0: archiving off — nothing written (no churn, count unchanged). */
    sqlite3_exec(db, "UPDATE config SET value='0' WHERE key='llm_response_archive_max';",
                 NULL, NULL, NULL);
    for (int i = 0; i < 3; i++)
        db_archive_response(db, sid, 300 + i, "m", "ok", "{\"x\":1}", NULL);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses;") == 9);

    db_close(db);
}

/* Capture: OpenRouter reasoning_details is stored verbatim and tagged with the
 * producing provider+model — even with reasoning_display 'none', because the replay
 * requirement is the provider's, not the operator's. */
static void test_capture_reasoning_details(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"listing\",\"reasoning\":\"because\","
        "\"reasoning_details\":[{\"type\":\"reasoning.text\",\"text\":\"because\","
        "\"format\":\"google-gemini-v1\",\"signature\":\"SIG123\"}],"
        "\"tool_calls\":[{\"id\":\"c1\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell_exec\",\"arguments\":\"{}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";
    assert(db_ingest_response(db, sid, 1, "gemini-3-flash", ENDPOINT_OPENAI,
                              body, NULL, 0, NULL) == LLM_RESP_OK);

    assert(meta_is(db, "$.format", "reasoning_details"));
    assert(meta_is(db, "$.model", "gemini-3-flash"));
    assert(meta_is(db, "$.provider", "openai"));
    assert(meta_is(db, "$.blob[0].signature", "SIG123"));
    /* Text not kept ('none'): the text is discarded, the blob is not. */
    assert(test_qi(db,
        "SELECT COUNT(*) FROM entries WHERE type='reasoning' AND content IS NULL") == 1);

    /* Bare-string shape (DeepSeek): the string itself is the blob. */
    const char *ds =
        "{\"choices\":[{\"message\":{\"content\":\"ok\","
        "\"reasoning_content\":\"deep thought\"},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":1,\"completion_tokens\":1}}";
    assert(db_ingest_response(db, sid, 2, "deepseek-v4", ENDPOINT_OPENAI,
                              ds, NULL, 1, NULL) == LLM_RESP_OK);
    assert(meta_is(db, "$.format", "reasoning_content"));
    assert(meta_is(db, "$.blob", "deep thought"));

    db_close(db);
}

/* The summary a route asks for (reasoning_display) is the reasoning entry's
 * text, in every format, and nowhere else: Gemini's thought parts leave the
 * answer, Anthropic's summarized thinking is the text while the content array
 * stays the blob. Not kept, the entry still carries the blob. Either way the
 * entry's size is its replay — the provider's reasoning count — never the
 * text's length. */
static void test_reasoning_summary_stored(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *gem =
        "{\"candidates\":[{\"content\":{\"role\":\"model\",\"parts\":["
        "{\"text\":\"**Plan**\\nCount the primes.\",\"thought\":true},"
        "{\"text\":\"Calling now.\"},"
        "{\"functionCall\":{\"name\":\"shell_exec\",\"args\":{}},"
        " \"thoughtSignature\":\"GSIG\"}]},\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":10,\"candidatesTokenCount\":5,"
        " \"thoughtsTokenCount\":40,\"totalTokenCount\":55}}";
    assert(db_ingest_response(db, sid, 1, "g@gemini", ENDPOINT_GEMINI,
                              gem, NULL, 1, NULL) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=1"
                          " AND type='reasoning' AND content='**Plan**' || char(10) ||"
                          " 'Count the primes.'") == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=1"
                          " AND type='assistant_message' AND content='Calling now.'") == 1);
    assert(meta_is(db, "$.blob[0].sig", "GSIG"));
    assert(test_qi(db, "SELECT tokens FROM entries WHERE iteration_id=1"
                          " AND type='reasoning'") == 40);
    assert(test_qi(db, "SELECT token_estimate FROM entries WHERE iteration_id=1"
                          " AND type='reasoning'") == TOKEN_ESTIMATE(0));

    const char *anth =
        "{\"type\":\"message\",\"content\":["
        "{\"type\":\"thinking\",\"thinking\":\"I list the primes: 17.\",\"signature\":\"ASIG\"},"
        "{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"shell_exec\",\"input\":{}}],"
        "\"stop_reason\":\"tool_use\",\"usage\":{\"input_tokens\":5,\"output_tokens\":30,"
        " \"output_tokens_details\":{\"thinking_tokens\":20}}}";
    assert(db_ingest_response(db, sid, 2, "c@anthropic", ENDPOINT_ANTHROPIC,
                              anth, NULL, 1, NULL) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=2"
                          " AND type='reasoning' AND content='I list the primes: 17.'") == 1);
    assert(meta_is(db, "$.blob[0].signature", "ASIG"));
    assert(test_qi(db, "SELECT token_estimate FROM entries WHERE iteration_id=2"
                          " AND type='reasoning'") == TOKEN_ESTIMATE(0));

    /* Not kept: the same responses leave no text, and the blob stays. */
    assert(db_ingest_response(db, sid, 3, "c@anthropic", ENDPOINT_ANTHROPIC,
                              anth, NULL, 0, NULL) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=3"
                          " AND type='reasoning' AND content IS NULL"
                          " AND json_extract(reasoning_meta,'$.blob[0].signature')='ASIG'") == 1);
    assert(db_ingest_response(db, sid, 4, "g@gemini", ENDPOINT_GEMINI,
                              gem, NULL, 0, NULL) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=4"
                          " AND type='reasoning' AND content IS NULL") == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=4"
                          " AND type='assistant_message' AND content='Calling now.'") == 1);

    db_close(db);
}

/* Capture: Gemini-native thoughtSignature, keyed by the functionCall it rode. */
static void test_capture_gemini_signature(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *body =
        "{\"candidates\":[{\"content\":{\"parts\":["
        "{\"text\":\"ok\"},"
        "{\"functionCall\":{\"name\":\"shell_exec\",\"args\":{\"cmd\":\"ls\"}},"
        " \"thoughtSignature\":\"GSIG\"}]},\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":1,\"candidatesTokenCount\":1}}";
    assert(db_ingest_response(db, sid, 1, "gemini-3-pro", ENDPOINT_GEMINI,
                              body, NULL, 0, NULL) == LLM_RESP_OK);

    assert(meta_is(db, "$.format", "gemini_parts"));
    assert(meta_is(db, "$.blob[0].fn", "shell_exec"));
    assert(meta_is(db, "$.blob[0].sig", "GSIG"));

    /* No signature anywhere → no reasoning entry at all. */
    const char *plain =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]},"
        "\"finishReason\":\"STOP\"}],"
        "\"usageMetadata\":{\"promptTokenCount\":1,\"candidatesTokenCount\":1}}";
    assert(db_ingest_response(db, sid, 2, "gemini-3-pro", ENDPOINT_GEMINI,
                              plain, NULL, 1, NULL) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE type='reasoning'") == 1);

    db_close(db);
}

/* ── Usage parity ── */

/* Every usage spelling lands in the same columns. resp/entry are conditions
 * on the iteration's llm_responses row and assistant entry; -1 skips an
 * in-memory count.
 * - OpenRouter: the full block, cost included; the cache-read count is
 *   mirrored onto the entry (limiter input).
 * - A bare three-field block leaves every new column NULL.
 * - DeepSeek-direct spells the cache hit prompt_cache_hit_tokens.
 * - Gemini: thoughts are added to the completion count (was undercounted),
 *   cachedContentTokenCount is the cache-read subset. Its JSON omits a
 *   zero-valued field (proto3), so a report without cachedContentTokenCount is
 *   a reported zero (seen live 2026-11-18 on a request whose 12k-token prefix
 *   the previous one had just read from cache); no usageMetadata at all stays
 *   not reported.
 * - Anthropic: a 1-hour cache write is a cache write. The API reports it split
 *   by lifetime under cache_creation and summed in cache_creation_input_tokens;
 *   the sum is stored, so the hour's tokens land in cache_write_tokens and the
 *   prompt count beside the 5-minute ones (shape from the prompt-caching docs,
 *   checked 2026-11-18). */
static const struct {
    EndpointType ep;
    const char *body;
    int64_t prompt, completion;
    const char *resp, *entry;
} usage[] = {
    { ENDPOINT_OPENAI,
      "{\"choices\":[{\"message\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
      "\"usage\":{\"prompt_tokens\":46153,\"completion_tokens\":65,"
      " \"cost\":0.0040173644,"
      " \"prompt_tokens_details\":{\"cached_tokens\":40000,\"cache_write_tokens\":128},"
      " \"completion_tokens_details\":{\"reasoning_tokens\":64}}}",
      46153, 65,
      "cached_tokens=40000 AND cache_write_tokens=128 AND reasoning_tokens=64"
      " AND CAST(cost*1e9 AS INTEGER)=4017364", "cached_tokens=40000" },
    { ENDPOINT_OPENAI,
      "{\"choices\":[{\"message\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
      "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":3,\"total_tokens\":13}}",
      10, 3,
      "cached_tokens IS NULL AND cache_write_tokens IS NULL AND reasoning_tokens IS NULL"
      " AND cost IS NULL", "cached_tokens IS NULL" },
    { ENDPOINT_OPENAI,
      "{\"choices\":[{\"message\":{\"content\":\"hi\"},\"finish_reason\":\"stop\"}],"
      "\"usage\":{\"prompt_tokens\":900,\"completion_tokens\":7,"
      " \"prompt_cache_hit_tokens\":768,\"prompt_cache_miss_tokens\":132}}",
      900, 7, "cached_tokens=768", "cached_tokens=768" },
    { ENDPOINT_GEMINI,
      "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]},\"finishReason\":\"STOP\"}],"
      "\"usageMetadata\":{\"promptTokenCount\":5000,\"candidatesTokenCount\":20,"
      " \"thoughtsTokenCount\":180,\"cachedContentTokenCount\":4096,\"totalTokenCount\":5200}}",
      5000, 200, "cached_tokens=4096 AND reasoning_tokens=180", "usage_out=200" },
    { ENDPOINT_GEMINI,
      "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]},\"finishReason\":\"STOP\"}],"
      "\"usageMetadata\":{\"promptTokenCount\":5000,\"candidatesTokenCount\":20,"
      " \"totalTokenCount\":5020}}",
      5000, 20, "cached_tokens=0", "cached_tokens=0" },
    { ENDPOINT_GEMINI,
      "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}]},\"finishReason\":\"STOP\"}]}",
      -1, -1, "cached_tokens IS NULL", "cached_tokens IS NULL" },
    { ENDPOINT_ANTHROPIC,
      "{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],"
      "\"stop_reason\":\"end_turn\","
      "\"usage\":{\"input_tokens\":2048,\"cache_read_input_tokens\":1800,"
      " \"cache_creation_input_tokens\":248,\"output_tokens\":503,"
      " \"cache_creation\":{\"ephemeral_5m_input_tokens\":148,\"ephemeral_1h_input_tokens\":100}}}",
      2048 + 1800 + 248, -1, "cached_tokens=1800 AND cache_write_tokens=248", "1" },
    { ENDPOINT_ANTHROPIC,
      "{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"ok\"}],"
      "\"stop_reason\":\"end_turn\","
      "\"usage\":{\"input_tokens\":5,\"cache_read_input_tokens\":0,"
      " \"cache_creation_input_tokens\":9000,\"output_tokens\":3,"
      " \"cache_creation\":{\"ephemeral_5m_input_tokens\":0,\"ephemeral_1h_input_tokens\":9000}}}",
      9005, -1, "cache_write_tokens=9000", "1" },
};

static void test_usage_formats(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    for (size_t i = 0; i < sizeof(usage) / sizeof(usage[0]); i++) {
        int64_t it = 10 + (int64_t)i;
        TypedIngestResult ir;
        int ok = db_ingest_response(db, sid, it, "m", usage[i].ep, usage[i].body,
                                    NULL, 1, &ir) == LLM_RESP_OK;
        ok = ok && (usage[i].prompt < 0 || ir.prompt_tokens == usage[i].prompt);
        ok = ok && (usage[i].completion < 0 || ir.completion_tokens == usage[i].completion);
        char *q = sqlite3_mprintf("SELECT COUNT(*) FROM llm_responses WHERE iteration_id=%lld"
                                  " AND (%s)", (long long)it, usage[i].resp);
        ok = ok && test_qi(db, q) == 1;
        sqlite3_free(q);
        q = sqlite3_mprintf("SELECT COUNT(*) FROM entries WHERE iteration_id=%lld"
                            " AND type='assistant_message' AND (%s)", (long long)it, usage[i].entry);
        ok = ok && test_qi(db, q) == 1;
        sqlite3_free(q);
        if (!ok) {
            fprintf(stderr, "FAIL usage[%zu]: prompt %lld completion %lld\n", i,
                    (long long)ir.prompt_tokens, (long long)ir.completion_tokens);
            abort();
        }
    }
    db_close(db);
}

/* rate_limit_check weights cache reads at 0.25; absent => full weight */
static void test_rate_limit_cache_discount(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    /* No cached_tokens: 1000 in + 100 out = 1100 counted (pre-M4 behavior). */
    entry_append_typed(db, sid, 1, "assistant_message", 0, "a", NULL, NULL, 0,
                       STOP_REASON_STOP, "m", 1000, 100, 0);
    assert(rate_limit_check(db, 1101) == 1);
    assert(rate_limit_check(db, 1100) == 0);

    /* 800 of the next 1000 input tokens were cache reads: 1000+100-600 = 500.
     * Running total 1600, not the cache-blind 2200. */
    int64_t e2 = entry_append_typed(db, sid, 2, "assistant_message", 0, "b", NULL, NULL, 0,
                                    STOP_REASON_STOP, "m", 1000, 100, 0);
    char up[128];
    snprintf(up, sizeof(up), "UPDATE entries SET cached_tokens=800 WHERE id=%lld;", (long long)e2);
    sqlite3_exec(db, up, NULL, NULL, NULL);
    assert(rate_limit_check(db, 1601) == 1);
    assert(rate_limit_check(db, 1600) == 0);

    /* limit <= 0 is unlimited regardless. */
    assert(rate_limit_check(db, 0) == 1);

    db_close(db);
}

/* Anthropic Messages — content blocks flatten into the same entries,
 * usage is normalized to cache-inclusive, and a response that thought keeps
 * its whole content array for replay. */
static void test_ingest_anthropic(void) {
    sqlite3 *db = test_db();
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    const char *body =
        "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
        "{\"type\":\"thinking\",\"thinking\":\"\",\"signature\":\"SIG\"},"
        "{\"type\":\"text\",\"text\":\"Checking.\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"shell_exec\","
        " \"input\":{\"command\":\"ls\"},\"caller\":{\"type\":\"direct\"}}],"
        "\"stop_reason\":\"tool_use\","
        "\"usage\":{\"input_tokens\":4,\"cache_read_input_tokens\":16000,"
        " \"cache_creation_input_tokens\":200,\"output_tokens\":90,"
        " \"output_tokens_details\":{\"thinking_tokens\":48}}}";
    TypedIngestResult ir;
    assert(db_ingest_response(db, sid, 20, "claude@anthropic", ENDPOINT_ANTHROPIC,
                              body, NULL, 0, &ir) == LLM_RESP_OK);
    assert(ir.prompt_tokens == 16204);     /* 4 + 16000 read + 200 written */
    assert(ir.completion_tokens == 90);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE iteration_id=20"
                       " AND cached_tokens=16000 AND cache_write_tokens=200"
                       " AND reasoning_tokens=48") == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE type='assistant_message'"
                          " AND content='Checking.' AND stop_reason=3;") == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE type='tool_call'"
                          " AND tool_call_id='toolu_1' AND tool_name='shell_exec'"
                          " AND json_extract(content,'$.command')='ls';") == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM tool_calls WHERE call_id='toolu_1';") == 1);

    assert(meta_is(db, "$.format", "anthropic_content"));
    assert(meta_is(db, "$.provider", "anthropic"));
    assert(meta_is(db, "$.blob[0].signature", "SIG"));
    assert(meta_is(db, "$.blob[2].caller.type", "direct"));

    /* No thinking: nothing to replay, so no reasoning entry at all. */
    const char *plain =
        "{\"type\":\"message\",\"content\":[{\"type\":\"text\",\"text\":\"hi\"}],"
        "\"stop_reason\":\"end_turn\",\"usage\":{\"input_tokens\":9,\"output_tokens\":2}}";
    assert(db_ingest_response(db, sid, 21, "claude@anthropic", ENDPOINT_ANTHROPIC,
                              plain, NULL, 0, &ir) == LLM_RESP_OK);
    assert(ir.prompt_tokens == 9);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE type='reasoning';") == 1);

    /* Thinking spent the whole max_tokens: an empty content array is a valid
     * response (a length stop), not a malformed one. */
    const char *spent =
        "{\"type\":\"message\",\"content\":[],\"stop_reason\":\"max_tokens\","
        "\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}";
    assert(db_ingest_response(db, sid, 22, "claude@anthropic", ENDPOINT_ANTHROPIC,
                              spent, NULL, 0, &ir) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=22"
                          " AND type='assistant_message' AND stop_reason=2;") == 1);

    /* A refusal is a 200 with nothing in it: the entry says what happened. */
    const char *refused =
        "{\"type\":\"message\",\"content\":[],\"stop_reason\":\"refusal\","
        "\"stop_details\":{\"type\":\"refusal\",\"category\":\"cyber\"},"
        "\"usage\":{\"input_tokens\":10,\"output_tokens\":0}}";
    assert(db_ingest_response(db, sid, 23, "claude@anthropic", ENDPOINT_ANTHROPIC,
                              refused, NULL, 0, &ir) == LLM_RESP_OK);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=23"
                          " AND content='[the model declined this request: cyber]';") == 1);

    /* An API error object is not a message. */
    assert(db_ingest_response(db, sid, 24, "claude@anthropic", ENDPOINT_ANTHROPIC,
               "{\"type\":\"error\",\"error\":{\"type\":\"overloaded_error\"}}",
               NULL, 0, &ir) == LLM_RESP_MALFORMED);

    db_close(db);
}

/* Another handle tries to commit while an ingest is between its reads and its
 * writes. The Gemini and Anthropic scalar queries read through json_each,
 * which opens a read transaction on the handle; held open across the writes
 * with no write lock, a commit from elsewhere left the ingest on a stale
 * snapshot and its writes failed at once (SQLITE_BUSY_SNAPSHOT, which never
 * reaches the busy handler) — a valid, paid-for response discarded. The
 * ingest now takes the write lock before it reads, so the other handle is the
 * one that waits. The trace hook fires the second handle's write at the
 * tool-call count, which runs after the scalar read and before any write. */
typedef struct { sqlite3 *other; int fired; int other_rc; } SnapProbe;

static int snap_trace(unsigned t, void *ctx, void *p, void *x) {
    (void)t; (void)x;
    SnapProbe *pr = ctx;
    const char *sql = sqlite3_sql((sqlite3_stmt *)p);
    if (pr->fired || !sql || strncmp(sql, "SELECT json_extract(value,'$.id')", 33) != 0) return 0;
    pr->fired = 1;
    pr->other_rc = sqlite3_exec(pr->other, "INSERT INTO other_writer VALUES(1);",
                                NULL, NULL, NULL);
    return 0;
}

static void test_ingest_concurrent_commit(void) {
    const char *path = "/tmp/cclaw_test_db_response_snap.db";
    remove(path);
    remove("/tmp/cclaw_test_db_response_snap.db-wal");
    remove("/tmp/cclaw_test_db_response_snap.db-shm");
    sqlite3 *db = test_db_open(path);
    assert(db);
    sqlite3_exec(db, "CREATE TABLE other_writer(x);", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    Message msg = {.role = ROLE_USER, .content = "do it"};
    entry_append_with_iteration(db, sid, &msg, 1);

    /* The second handle does not wait: it reports what it met. */
    sqlite3 *other = db_open(path);
    assert(other);
    sqlite3_busy_handler(other, NULL, NULL);
    SnapProbe pr = { other, 0, -1 };
    sqlite3_trace_v2(db, SQLITE_TRACE_STMT, snap_trace, &pr);

    const char *body =
        "{\"id\":\"msg_1\",\"type\":\"message\",\"role\":\"assistant\",\"content\":["
        "{\"type\":\"text\",\"text\":\"Checking.\"},"
        "{\"type\":\"tool_use\",\"id\":\"toolu_1\",\"name\":\"shell_exec\","
        " \"input\":{\"command\":\"ls\"}}],"
        "\"stop_reason\":\"tool_use\","
        "\"usage\":{\"input_tokens\":40,\"output_tokens\":9}}";
    TypedIngestResult ir;
    LlmRespStatus st = db_ingest_response(db, sid, 2, "claude@anthropic", ENDPOINT_ANTHROPIC,
                                          body, "{\"model\":\"claude\"}", 0, &ir);
    sqlite3_trace_v2(db, 0, NULL, NULL);
    assert(pr.fired);
    assert(st == LLM_RESP_OK);
    /* The write lock was already held when the other handle tried. */
    assert((pr.other_rc & 0xff) == SQLITE_BUSY);
    assert(test_qi(db, "SELECT COUNT(*) FROM llm_responses WHERE status='ok'") == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM entries WHERE iteration_id=2"
                          " AND type='assistant_message'") == 1);
    assert(test_qi(db, "SELECT COUNT(*) FROM tool_calls WHERE call_id='toolu_1'") == 1);
    /* And it is released: the other handle writes freely afterwards. */
    assert(sqlite3_exec(other, "INSERT INTO other_writer VALUES(2);", NULL, NULL, NULL) == SQLITE_OK);
    /* The request is archived as the text that was sent, not re-parsed. */
    assert(test_qi(db, "SELECT typeof(request_body)='text' AND"
                          " request_body ->> '$.model' = 'claude' FROM llm_responses") == 1);

    sqlite3_close(other);
    db_close(db);
    remove(path);
    remove("/tmp/cclaw_test_db_response_snap.db-wal");
    remove("/tmp/cclaw_test_db_response_snap.db-shm");
}

int main(void) {
    TEST_INIT();
    printf("test_db_response:\n");
    RUN(test_ingest_response);
    RUN(test_ingest_malformed);
    RUN(test_ingest_archive);
    RUN(test_archive_retention);
    RUN(test_capture_reasoning_details);
    RUN(test_capture_gemini_signature);
    RUN(test_reasoning_summary_stored);
    RUN(test_usage_formats);
    RUN(test_ingest_anthropic);
    RUN(test_rate_limit_cache_discount);
    RUN(test_ingest_concurrent_commit);
    return 0;
}
