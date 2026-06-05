#include "sse_parse.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ─── OpenAI tests ───────────────────────────────────────────────────── */

static void test_openai_content_delta(void) {
    const char *json =
        "{\"id\":\"chatcmpl-abc\",\"choices\":[{\"index\":0,\"delta\":"
        "{\"content\":\"Hello world\"},\"finish_reason\":null}]}";
    SseChunk c;
    int rc = sse_parse_openai(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.text_len == 11);
    assert(memcmp(c.text, "Hello world", 11) == 0);
    assert(c.finish == NULL);
    assert(c.tc_index == -1);
    printf("  PASS: openai_content_delta\n");
}

static void test_openai_reasoning_delta(void) {
    const char *json =
        "{\"choices\":[{\"index\":0,\"delta\":"
        "{\"reasoning_content\":\"thinking\"},\"finish_reason\":null}]}";
    SseChunk c;
    int rc = sse_parse_openai(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.reasoning_len == 8);
    assert(memcmp(c.reasoning, "thinking", 8) == 0);
    assert(c.text == NULL);
    printf("  PASS: openai_reasoning_delta\n");
}

static void test_openai_reasoning_field(void) {
    /* Some providers use "reasoning" instead of "reasoning_content" */
    const char *json =
        "{\"choices\":[{\"index\":0,\"delta\":"
        "{\"reasoning\":\"Let me think\"},\"finish_reason\":null}]}";
    SseChunk c;
    int rc = sse_parse_openai(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.reasoning_len == 12);
    assert(memcmp(c.reasoning, "Let me think", 12) == 0);
    printf("  PASS: openai_reasoning_field\n");
}

static void test_openai_tool_call_start(void) {
    const char *json =
        "{\"choices\":[{\"index\":0,\"delta\":"
        "{\"tool_calls\":[{\"index\":0,\"id\":\"call_abc123\","
        "\"function\":{\"name\":\"get_weather\",\"arguments\":\"\"}}]},"
        "\"finish_reason\":null}]}";
    SseChunk c;
    int rc = sse_parse_openai(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.tc_index == 0);
    assert(c.tc_id_len == 11);
    assert(memcmp(c.tc_id, "call_abc123", 11) == 0);
    assert(c.tc_name_len == 11);
    assert(memcmp(c.tc_name, "get_weather", 11) == 0);
    printf("  PASS: openai_tool_call_start\n");
}

static void test_openai_tool_call_args_fragment(void) {
    const char *json =
        "{\"choices\":[{\"index\":0,\"delta\":"
        "{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\\\"loc\"}}]},"
        "\"finish_reason\":null}]}";
    SseChunk c;
    int rc = sse_parse_openai(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.tc_index == 0);
    /* Raw JSON string content: {\"loc = 6 bytes (unescape yields {"loc = 5) */
    assert(c.tc_args_len == 6);
    assert(c.tc_args_complete == 0);
    printf("  PASS: openai_tool_call_args_fragment\n");
}

static void test_openai_finish_reason(void) {
    const char *json =
        "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}";
    SseChunk c;
    int rc = sse_parse_openai(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.finish_len == 4);
    assert(memcmp(c.finish, "stop", 4) == 0);
    printf("  PASS: openai_finish_reason\n");
}

static void test_openai_usage(void) {
    const char *json =
        "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":42,\"completion_tokens\":10,\"total_tokens\":52,"
        "\"prompt_tokens_details\":{\"cached_tokens\":20,\"cache_write_tokens\":5},"
        "\"cost\":0.000123}}";
    SseChunk c;
    int rc = sse_parse_openai(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.prompt_tokens == 42);
    assert(c.completion_tokens == 10);
    assert(c.total_tokens == 52);
    assert(c.cache_read_tokens == 20);
    assert(c.cache_write_tokens == 5);
    assert(c.cost_nano == 123000);
    printf("  PASS: openai_usage\n");
}

static void test_openai_empty_delta(void) {
    const char *json = "{\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":null}]}";
    SseChunk c;
    int rc = sse_parse_openai(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.text == NULL);
    assert(c.tc_index == -1);
    printf("  PASS: openai_empty_delta\n");
}

static void test_openai_invalid_json(void) {
    /* jsmn returns error code (<= 0) for malformed JSON */
    const char *json = "";
    SseChunk c;
    int rc = sse_parse_openai(json, 0, &c);
    assert(rc == -1);
    printf("  PASS: openai_invalid_json\n");
}

/* ─── Gemini tests ───────────────────────────────────────────────────── */

static void test_gemini_text_content(void) {
    const char *json =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Hello\"}],"
        "\"role\":\"model\"},\"index\":0}]}";
    SseChunk c;
    int rc = sse_parse_gemini(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.text_len == 5);
    assert(memcmp(c.text, "Hello", 5) == 0);
    assert(c.reasoning == NULL);
    printf("  PASS: gemini_text_content\n");
}

static void test_gemini_thought(void) {
    const char *json =
        "{\"candidates\":[{\"content\":{\"parts\":["
        "{\"text\":\"thinking\",\"thought\":true}],"
        "\"role\":\"model\"},\"index\":0}]}";
    SseChunk c;
    int rc = sse_parse_gemini(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.reasoning_len == 8);
    assert(memcmp(c.reasoning, "thinking", 8) == 0);
    assert(c.text == NULL);
    printf("  PASS: gemini_thought\n");
}

static void test_gemini_function_call(void) {
    const char *json =
        "{\"candidates\":[{\"content\":{\"parts\":["
        "{\"functionCall\":{\"name\":\"get_weather\",\"args\":{\"loc\":\"NYC\"}}}],"
        "\"role\":\"model\"},\"index\":0}]}";
    SseChunk c;
    int rc = sse_parse_gemini(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.tc_name_len == 11);
    assert(memcmp(c.tc_name, "get_weather", 11) == 0);
    assert(c.tc_args_complete == 1);
    /* {"loc":"NYC"} = 13 chars */
    assert(c.tc_args_len == 13);
    assert(memcmp(c.tc_args, "{\"loc\":\"NYC\"}", 13) == 0);
    printf("  PASS: gemini_function_call\n");
}

static void test_gemini_finish_reason(void) {
    const char *json =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"done\"}],"
        "\"role\":\"model\"},\"finishReason\":\"STOP\",\"index\":0}]}";
    SseChunk c;
    int rc = sse_parse_gemini(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.finish_len == 4);
    assert(memcmp(c.finish, "STOP", 4) == 0);
    printf("  PASS: gemini_finish_reason\n");
}

static void test_gemini_usage(void) {
    const char *json =
        "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hi\"}],"
        "\"role\":\"model\"},\"index\":0}],"
        "\"usageMetadata\":{\"promptTokenCount\":100,"
        "\"candidatesTokenCount\":20,\"totalTokenCount\":120,"
        "\"cachedContentTokenCount\":50}}";
    SseChunk c;
    int rc = sse_parse_gemini(json, strlen(json), &c);
    assert(rc == 0);
    assert(c.prompt_tokens == 100);
    assert(c.completion_tokens == 20);
    assert(c.total_tokens == 120);
    assert(c.cache_read_tokens == 50);
    printf("  PASS: gemini_usage\n");
}

/* ─── Anthropic tests ────────────────────────────────────────────────── */

static void test_anthropic_text_delta(void) {
    const char *json =
        "{\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello\"}}";
    SseChunk c;
    int rc = sse_parse_anthropic("content_block_delta", json, strlen(json), &c);
    assert(rc == 0);
    assert(c.text_len == 5);
    assert(memcmp(c.text, "Hello", 5) == 0);
    assert(c.tc_index == 0);
    printf("  PASS: anthropic_text_delta\n");
}

static void test_anthropic_thinking_delta(void) {
    const char *json =
        "{\"type\":\"content_block_delta\",\"index\":1,"
        "\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"Let me\"}}";
    SseChunk c;
    int rc = sse_parse_anthropic("content_block_delta", json, strlen(json), &c);
    assert(rc == 0);
    assert(c.reasoning_len == 6);
    assert(memcmp(c.reasoning, "Let me", 6) == 0);
    assert(c.tc_index == 1);
    printf("  PASS: anthropic_thinking_delta\n");
}

static void test_anthropic_tool_use_start(void) {
    const char *json =
        "{\"type\":\"content_block_start\",\"index\":2,"
        "\"content_block\":{\"type\":\"tool_use\",\"id\":\"toolu_abc\","
        "\"name\":\"get_weather\"}}";
    SseChunk c;
    int rc = sse_parse_anthropic("content_block_start", json, strlen(json), &c);
    assert(rc == 0);
    assert(c.tc_id_len == 9);
    assert(memcmp(c.tc_id, "toolu_abc", 9) == 0);
    assert(c.tc_name_len == 11);
    assert(memcmp(c.tc_name, "get_weather", 11) == 0);
    assert(c.tc_index == 2);
    printf("  PASS: anthropic_tool_use_start\n");
}

static void test_anthropic_input_json_delta(void) {
    const char *json =
        "{\"type\":\"content_block_delta\",\"index\":2,"
        "\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"loc\"}}";
    SseChunk c;
    int rc = sse_parse_anthropic("content_block_delta", json, strlen(json), &c);
    assert(rc == 0);
    assert(c.tc_args != NULL);
    /* Raw JSON string content: {\"loc = 6 bytes */
    assert(c.tc_args_len == 6);
    assert(c.tc_index == 2);
    printf("  PASS: anthropic_input_json_delta\n");
}

static void test_anthropic_message_delta(void) {
    const char *json =
        "{\"type\":\"message_delta\","
        "\"delta\":{\"stop_reason\":\"end_turn\"},"
        "\"usage\":{\"output_tokens\":15}}";
    SseChunk c;
    int rc = sse_parse_anthropic("message_delta", json, strlen(json), &c);
    assert(rc == 0);
    assert(c.finish_len == 8);
    assert(memcmp(c.finish, "end_turn", 8) == 0);
    assert(c.completion_tokens == 15);
    printf("  PASS: anthropic_message_delta\n");
}

static void test_anthropic_message_start(void) {
    const char *json =
        "{\"type\":\"message_start\",\"message\":{\"id\":\"msg_abc\","
        "\"usage\":{\"input_tokens\":42,\"cache_read_input_tokens\":20,"
        "\"cache_creation_input_tokens\":5}}}";
    SseChunk c;
    int rc = sse_parse_anthropic("message_start", json, strlen(json), &c);
    assert(rc == 0);
    assert(c.prompt_tokens == 42);
    assert(c.cache_read_tokens == 20);
    assert(c.cache_write_tokens == 5);
    printf("  PASS: anthropic_message_start\n");
}

int main(void) {
    printf("--- test_sse_parse ---\n");

    /* OpenAI */
    test_openai_content_delta();
    test_openai_reasoning_delta();
    test_openai_reasoning_field();
    test_openai_tool_call_start();
    test_openai_tool_call_args_fragment();
    test_openai_finish_reason();
    test_openai_usage();
    test_openai_empty_delta();
    test_openai_invalid_json();

    /* Gemini */
    test_gemini_text_content();
    test_gemini_thought();
    test_gemini_function_call();
    test_gemini_finish_reason();
    test_gemini_usage();

    /* Anthropic */
    test_anthropic_text_delta();
    test_anthropic_thinking_delta();
    test_anthropic_tool_use_start();
    test_anthropic_input_json_delta();
    test_anthropic_message_delta();
    test_anthropic_message_start();

    printf("20/20 passed\n");
    return 0;
}
