#include <stdio.h>
#include <string.h>
#include "llm.h"
#include "cJSON.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static void test_parse_content_response(void) {
    TEST(parse_content_response);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);

    const char *json =
        "{\"choices\":[{\"message\":{\"content\":\"Hello world\",\"role\":\"assistant\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":10,\"completion_tokens\":5,\"total_tokens\":15}}";

    LlmResponse resp;
    int rc = llm_parse_response(a, json, &resp);
    if (rc != 0) { FAIL("parse failed"); arena_destroy(a); return; }
    if (!resp.content || strcmp(resp.content, "Hello world") != 0) {
        FAIL("wrong content"); arena_destroy(a); return;
    }
    if (resp.tool_call_count != 0) { FAIL("unexpected tool_calls"); arena_destroy(a); return; }
    if (resp.usage.prompt_tokens != 10 || resp.usage.completion_tokens != 5 || resp.usage.total_tokens != 15) {
        FAIL("wrong usage"); arena_destroy(a); return;
    }
    if (!resp.finish_reason || strcmp(resp.finish_reason, "stop") != 0) {
        FAIL("wrong finish_reason"); arena_destroy(a); return;
    }

    arena_destroy(a);
    PASS();
}

static void test_parse_tool_calls_response(void) {
    TEST(parse_tool_calls_response);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);

    const char *json =
        "{\"choices\":[{\"message\":{\"content\":null,\"role\":\"assistant\","
        "\"tool_calls\":[{\"id\":\"call_abc\",\"type\":\"function\","
        "\"function\":{\"name\":\"shell_exec\",\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":20,\"completion_tokens\":10,\"total_tokens\":30}}";

    LlmResponse resp;
    int rc = llm_parse_response(a, json, &resp);
    if (rc != 0) { FAIL("parse failed"); arena_destroy(a); return; }
    if (resp.content != NULL) { FAIL("content should be NULL"); arena_destroy(a); return; }
    if (resp.tool_call_count != 1) { FAIL("wrong tool_call_count"); arena_destroy(a); return; }
    if (strcmp(resp.tool_calls[0].id, "call_abc") != 0) { FAIL("wrong tc id"); arena_destroy(a); return; }
    if (strcmp(resp.tool_calls[0].name, "shell_exec") != 0) { FAIL("wrong tc name"); arena_destroy(a); return; }
    if (strcmp(resp.tool_calls[0].arguments, "{\"cmd\":\"ls\"}") != 0) { FAIL("wrong tc args"); arena_destroy(a); return; }
    if (!resp.finish_reason || strcmp(resp.finish_reason, "tool_calls") != 0) {
        FAIL("wrong finish_reason"); arena_destroy(a); return;
    }

    arena_destroy(a);
    PASS();
}

static void test_parse_invalid_json(void) {
    TEST(parse_invalid_json);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);

    LlmResponse resp;
    int rc = llm_parse_response(a, "not json", &resp);
    if (rc != -1) { FAIL("should fail on invalid JSON"); arena_destroy(a); return; }

    rc = llm_parse_response(a, NULL, &resp);
    if (rc != -1) { FAIL("should fail on NULL"); arena_destroy(a); return; }

    rc = llm_parse_response(a, "{\"choices\":[]}", &resp);
    if (rc != -1) { FAIL("should fail on empty choices"); arena_destroy(a); return; }

    arena_destroy(a);
    PASS();
}

static void test_parse_cost_field(void) {
    TEST(parse_cost_field);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);

    const char *json =
        "{\"choices\":[{\"message\":{\"content\":\"hi\",\"role\":\"assistant\"},"
        "\"finish_reason\":\"stop\"}],"
        "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":5,\"total_tokens\":105,"
        "\"cost\":0.000072112}}";

    LlmResponse resp;
    int rc = llm_parse_response(a, json, &resp);
    if (rc != 0) { FAIL("parse failed"); arena_destroy(a); return; }
    if (resp.usage.cost_nano != 72112) {
        char buf[64]; snprintf(buf, sizeof(buf), "expected 72112, got %lld", (long long)resp.usage.cost_nano);
        FAIL(buf); arena_destroy(a); return;
    }
    arena_destroy(a);
    PASS();
}

static void test_map_stop_reason(void) {
    TEST(map_stop_reason);
    if (map_stop_reason("stop") != STOP_REASON_STOP) { FAIL("stop"); return; }
    if (map_stop_reason("end_turn") != STOP_REASON_STOP) { FAIL("end_turn"); return; }
    if (map_stop_reason("length") != STOP_REASON_LENGTH) { FAIL("length"); return; }
    if (map_stop_reason("max_tokens") != STOP_REASON_LENGTH) { FAIL("max_tokens"); return; }
    if (map_stop_reason("tool_calls") != STOP_REASON_TOOL_USE) { FAIL("tool_calls"); return; }
    if (map_stop_reason("tool_use") != STOP_REASON_TOOL_USE) { FAIL("tool_use"); return; }
    if (map_stop_reason("content_filter") != STOP_REASON_ERROR) { FAIL("content_filter"); return; }
    if (map_stop_reason(NULL) != STOP_REASON_STOP) { FAIL("NULL"); return; }
    if (map_stop_reason("unknown_thing") != STOP_REASON_ERROR) { FAIL("unknown"); return; }
    PASS();
}

int main(void) {
    printf("--- test_llm ---\n");
    test_parse_content_response();
    test_parse_tool_calls_response();
    test_parse_invalid_json();
    test_parse_cost_field();
    test_map_stop_reason();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
