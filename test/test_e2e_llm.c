#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llm.h"
#include "http.h"
#include "arena.h"
#include "cJSON.h"
#include <curl/curl.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define SKIP(msg) do { tests_passed++; printf("SKIP: %s\n", msg); return; } while(0)

static const char *get_api_key(void) {
    return getenv("OPENROUTER_API_KEY");
}

/* Call LLM and return parsed response. Returns 0 on success. */
static int call_llm(Arena *a, const Config *cfg, const Message *msgs,
                    size_t msg_count, const ToolSchema *tools,
                    size_t tool_count, LlmResponse *out) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON_AddStringToObject(root, "model", cfg->provider.model);
    if (cfg->provider.max_tokens > 0)
        cJSON_AddNumberToObject(root, "max_tokens", cfg->provider.max_tokens);

    cJSON *jarr = cJSON_AddArrayToObject(root, "messages");
    for (size_t i = 0; i < msg_count; i++) {
        cJSON *m = cJSON_CreateObject();
        const char *role = msgs[i].role == ROLE_SYSTEM ? "system" :
                           msgs[i].role == ROLE_USER ? "user" :
                           msgs[i].role == ROLE_ASSISTANT ? "assistant" : "tool";
        cJSON_AddStringToObject(m, "role", role);
        cJSON_AddStringToObject(m, "content", msgs[i].content ? msgs[i].content : "");
        cJSON_AddItemToArray(jarr, m);
    }

    if (tools && tool_count > 0) {
        cJSON *tarr = cJSON_AddArrayToObject(root, "tools");
        for (size_t i = 0; i < tool_count; i++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddStringToObject(t, "type", "function");
            cJSON *fn = cJSON_CreateObject();
            cJSON_AddStringToObject(fn, "name", tools[i].name);
            if (tools[i].description)
                cJSON_AddStringToObject(fn, "description", tools[i].description);
            if (tools[i].parameters_json) {
                cJSON *p = cJSON_Parse(tools[i].parameters_json);
                if (p) cJSON_AddItemToObject(fn, "parameters", p);
            }
            cJSON_AddItemToObject(t, "function", fn);
            cJSON_AddItemToArray(tarr, t);
        }
    }

    char *req_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!req_json) return -1;

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", cfg->provider.api_key);
    const char *headers[] = {
        "Content-Type: application/json",
        auth_hdr,
        NULL
    };

    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", cfg->provider.base_url);

    HttpResponse resp = {0};
    int status = http_post(url, headers, req_json, &resp);
    free(req_json);
    if (status != 200) {
        fprintf(stderr, "    HTTP %d: %.*s\n", status,
                (int)(resp.len > 200 ? 200 : resp.len), resp.data ? resp.data : "");
        http_response_free(&resp);
        return -1;
    }

    int rc = llm_parse_response(a, resp.data, out);
    http_response_free(&resp);
    return rc;
}

/* T52: live call with simple prompt, verify content response */
static void test_live_content_response(void) {
    TEST(live_content_response);
    const char *key = get_api_key();
    if (!key) SKIP("OPENROUTER_API_KEY not set");

    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = {0};
    cfg.provider.base_url = "https://openrouter.ai/api/v1";
    cfg.provider.api_key = (char *)key;
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.max_tokens = 64;
    cfg.provider.context_window = 128000;

    Message msgs[1] = {{ .role = ROLE_USER, .content = "Reply with exactly: hello" }};

    LlmResponse resp;
    int rc = call_llm(a, &cfg, msgs, 1, NULL, 0, &resp);
    if (rc != 0) { arena_destroy(a); FAIL("LLM call failed"); }
    if (!resp.content || strlen(resp.content) == 0) { arena_destroy(a); FAIL("empty content"); }
    if (resp.usage.total_tokens == 0) { arena_destroy(a); FAIL("no usage reported"); }

    arena_destroy(a);
    PASS();
}

/* T52: live call with tool, verify tool_calls round-trip */
static void test_live_tool_call_roundtrip(void) {
    TEST(live_tool_call_roundtrip);
    const char *key = get_api_key();
    if (!key) SKIP("OPENROUTER_API_KEY not set");

    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = {0};
    cfg.provider.base_url = "https://openrouter.ai/api/v1";
    cfg.provider.api_key = (char *)key;
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;

    Message msgs[2] = {
        { .role = ROLE_SYSTEM, .content = "You must use the get_weather tool to answer weather questions. Always call the tool." },
        { .role = ROLE_USER, .content = "What is the weather in Tokyo?" },
    };

    ToolSchema tools[1] = {{
        .name = "get_weather",
        .description = "Get current weather for a city",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"city\":{\"type\":\"string\",\"description\":\"City name\"}},\"required\":[\"city\"]}"
    }};

    LlmResponse resp;
    int rc = call_llm(a, &cfg, msgs, 2, tools, 1, &resp);
    if (rc != 0) { arena_destroy(a); FAIL("LLM call failed"); }
    if (resp.tool_call_count == 0) { arena_destroy(a); FAIL("no tool_calls returned"); }
    if (!resp.tool_calls[0].id || strlen(resp.tool_calls[0].id) == 0) {
        arena_destroy(a); FAIL("empty tool_call id");
    }
    if (strcmp(resp.tool_calls[0].name, "get_weather") != 0) {
        arena_destroy(a); FAIL("wrong tool name");
    }
    if (!resp.tool_calls[0].arguments || strlen(resp.tool_calls[0].arguments) == 0) {
        arena_destroy(a); FAIL("empty arguments");
    }

    arena_destroy(a);
    PASS();
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    printf("--- test_integration_llm ---\n");
    test_live_content_response();
    test_live_tool_call_roundtrip();
    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
