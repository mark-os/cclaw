#include <stdio.h>
#include <string.h>
#include "llm.h"
#include "cJSON.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static Config make_cfg(void) {
    Config cfg = {0};
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.base_url = "https://openrouter.ai/api/v1";
    cfg.provider.api_key = "sk-test";
    cfg.provider.max_tokens = 4096;
    cfg.provider.context_window = 128000;
    return cfg;
}

static void test_basic_request(void) {
    TEST(basic_request);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = make_cfg();
    Message msgs[2] = {
        { .role = ROLE_SYSTEM, .content = "You are helpful." },
        { .role = ROLE_USER, .content = "Hello" },
    };

    char *json = llm_build_request(a, &cfg, msgs, 2, NULL, 0);
    if (!json) { FAIL("returned NULL"); arena_destroy(a); return; }

    cJSON *root = cJSON_Parse(json);
    if (!root) { FAIL("invalid JSON"); arena_destroy(a); return; }

    cJSON *model = cJSON_GetObjectItem(root, "model");
    if (!model || strcmp(model->valuestring, "deepseek/deepseek-v4-flash") != 0) {
        FAIL("wrong model"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    if (!messages || cJSON_GetArraySize(messages) != 2) {
        FAIL("wrong message count"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON *max_tok = cJSON_GetObjectItem(root, "max_tokens");
    if (!max_tok || max_tok->valueint != 4096) {
        FAIL("wrong max_tokens"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON_Delete(root);
    arena_destroy(a);
    PASS();
}

static void test_v9_no_tools_field(void) {
    TEST(v9_no_tools_field);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = make_cfg();
    Message msgs[1] = {{ .role = ROLE_USER, .content = "Hi" }};

    char *json = llm_build_request(a, &cfg, msgs, 1, NULL, 0);
    if (!json) { FAIL("returned NULL"); arena_destroy(a); return; }

    cJSON *root = cJSON_Parse(json);
    if (cJSON_GetObjectItem(root, "tools")) {
        FAIL("tools field present when no tools"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON_Delete(root);
    arena_destroy(a);
    PASS();
}

static void test_with_tools(void) {
    TEST(with_tools);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = make_cfg();
    Message msgs[1] = {{ .role = ROLE_USER, .content = "list files" }};

    ToolSchema tools[1] = {{
        .name = "shell_exec",
        .description = "Run a shell command",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"cmd\":{\"type\":\"string\"}}}"
    }};

    char *json = llm_build_request(a, &cfg, msgs, 1, tools, 1);
    if (!json) { FAIL("returned NULL"); arena_destroy(a); return; }

    cJSON *root = cJSON_Parse(json);
    cJSON *tools_arr = cJSON_GetObjectItem(root, "tools");
    if (!tools_arr || cJSON_GetArraySize(tools_arr) != 1) {
        FAIL("tools array wrong"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON *t0 = cJSON_GetArrayItem(tools_arr, 0);
    cJSON *type = cJSON_GetObjectItem(t0, "type");
    if (!type || strcmp(type->valuestring, "function") != 0) {
        FAIL("tool type wrong"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON *fn = cJSON_GetObjectItem(t0, "function");
    cJSON *name = cJSON_GetObjectItem(fn, "name");
    if (!name || strcmp(name->valuestring, "shell_exec") != 0) {
        FAIL("tool name wrong"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON_Delete(root);
    arena_destroy(a);
    PASS();
}

static void test_tool_calls_message(void) {
    TEST(tool_calls_message);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = make_cfg();

    ToolCall tc = { .id = "call_123", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}" };
    Message msgs[2] = {
        { .role = ROLE_USER, .content = "list files" },
        { .role = ROLE_ASSISTANT, .content = NULL, .tool_calls = &tc, .tool_call_count = 1 },
    };

    char *json = llm_build_request(a, &cfg, msgs, 2, NULL, 0);
    if (!json) { FAIL("returned NULL"); arena_destroy(a); return; }

    cJSON *root = cJSON_Parse(json);
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    cJSON *asst = cJSON_GetArrayItem(messages, 1);
    cJSON *tcs = cJSON_GetObjectItem(asst, "tool_calls");
    if (!tcs || cJSON_GetArraySize(tcs) != 1) {
        FAIL("tool_calls missing"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON *tc0 = cJSON_GetArrayItem(tcs, 0);
    cJSON *id = cJSON_GetObjectItem(tc0, "id");
    if (!id || strcmp(id->valuestring, "call_123") != 0) {
        FAIL("tool call id wrong"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON_Delete(root);
    arena_destroy(a);
    PASS();
}

static void test_tool_result_message(void) {
    TEST(tool_result_message);
    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = make_cfg();

    ToolResult tr = { .tool_call_id = "call_123", .content = "file1.txt\nfile2.txt" };
    Message msgs[1] = {
        { .role = ROLE_TOOL, .content = NULL, .tool_result = &tr },
    };

    char *json = llm_build_request(a, &cfg, msgs, 1, NULL, 0);
    if (!json) { FAIL("returned NULL"); arena_destroy(a); return; }

    cJSON *root = cJSON_Parse(json);
    cJSON *messages = cJSON_GetObjectItem(root, "messages");
    cJSON *tool_msg = cJSON_GetArrayItem(messages, 0);
    cJSON *tcid = cJSON_GetObjectItem(tool_msg, "tool_call_id");
    if (!tcid || strcmp(tcid->valuestring, "call_123") != 0) {
        FAIL("tool_call_id wrong"); cJSON_Delete(root); arena_destroy(a); return;
    }
    cJSON *content = cJSON_GetObjectItem(tool_msg, "content");
    if (!content || strcmp(content->valuestring, "file1.txt\nfile2.txt") != 0) {
        FAIL("content wrong"); cJSON_Delete(root); arena_destroy(a); return;
    }

    cJSON_Delete(root);
    arena_destroy(a);
    PASS();
}

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

int main(void) {
    printf("--- test_llm ---\n");
    test_basic_request();
    test_v9_no_tools_field();
    test_with_tools();
    test_tool_calls_message();
    test_tool_result_message();
    test_parse_content_response();
    test_parse_tool_calls_response();
    test_parse_invalid_json();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
