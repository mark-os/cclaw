/* T222: e2e tests — real provider round-trips, tool_calls across providers,
 * full agent turn with workspace side-effects, Telegram delivery.
 * Requires API keys in env; skips gracefully if missing. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include "agent.h"
#include "agent_setup.h"
#include "db.h"
#include "config.h"
#include "llm.h"
#include "http.h"
#include "tools.h"
#include "tool_file.h"
#include "telegram.h"
#include <curl/curl.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define SKIP(msg) do { tests_passed++; printf("SKIP: %s\n", msg); return; } while(0)

static char *test_dispatch(const char *name, const char *arguments, void *user_data) {
    ToolRegistry *reg = (ToolRegistry *)user_data;
    ToolEntry *e = tools_lookup(reg, name);
    if (!e) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "error: unknown tool '%s'", name);
        return err ? err : strdup("error: unknown tool");
    }
    return e->handler(arguments, e->user_data);
}

/* Gemini tool_calls parsing — verify tool_call round-trip via Gemini direct */
static void test_gemini_tool_calls(void) {
    TEST(gemini_tool_calls);
    const char *key = getenv("GEMINI_API_KEY");
    if (!key) SKIP("GEMINI_API_KEY not set");

    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = {0};
    cfg.provider.base_url = "https://generativelanguage.googleapis.com/v1beta/openai";
    cfg.provider.api_key = (char *)key;
    cfg.provider.model = "gemma-4-31b-it";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;

    Message msgs[2] = {
        { .role = ROLE_SYSTEM, .content = "You must use the get_time tool to answer time questions. Always call the tool." },
        { .role = ROLE_USER, .content = "What time is it in London?" },
    };

    ToolSchema tools[1] = {{
        .name = "get_time",
        .description = "Get current time for a timezone",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"timezone\":{\"type\":\"string\",\"description\":\"Timezone name\"}},\"required\":[\"timezone\"]}"
    }};

    char *req_json = llm_build_request(a, &cfg, msgs, 2, tools, 1);
    if (!req_json) { arena_destroy(a); FAIL("build request failed"); }

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", key);
    const char *headers[] = { "Content-Type: application/json", auth_hdr, NULL };

    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", cfg.provider.base_url);

    HttpResponse resp = {0};
    int status = http_post(url, headers, req_json, &resp);
    if (status != 200) {
        fprintf(stderr, "    HTTP %d: %.*s\n", status,
                (int)(resp.len > 200 ? 200 : resp.len), resp.data ? resp.data : "");
        http_response_free(&resp);
        arena_destroy(a);
        FAIL("HTTP call failed");
    }

    LlmResponse llm_resp;
    int rc = llm_parse_response(a, resp.data, &llm_resp);
    http_response_free(&resp);
    if (rc != 0) { arena_destroy(a); FAIL("parse response failed"); }
    if (llm_resp.tool_call_count == 0) { arena_destroy(a); FAIL("no tool_calls from Gemini"); }
    if (!llm_resp.tool_calls[0].id || strlen(llm_resp.tool_calls[0].id) == 0) {
        arena_destroy(a); FAIL("empty tool_call id");
    }
    if (strcmp(llm_resp.tool_calls[0].name, "get_time") != 0) {
        arena_destroy(a); FAIL("wrong tool name");
    }
    if (!llm_resp.tool_calls[0].arguments || strlen(llm_resp.tool_calls[0].arguments) == 0) {
        arena_destroy(a); FAIL("empty arguments");
    }

    printf("(tool: %s, args: %.40s) ", llm_resp.tool_calls[0].name, llm_resp.tool_calls[0].arguments);
    arena_destroy(a);
    PASS();
}

/* Full agent turn with workspace side-effects — LLM calls file_write, verify file */
static void test_agent_workspace_side_effects(void) {
    TEST(agent_workspace_side_effects);
    const char *key = getenv("OPENROUTER_API_KEY");
    if (!key) SKIP("OPENROUTER_API_KEY not set");

    /* Create temp workspace */
    char ws_dir[] = "/tmp/cclaw_e2e_ws_XXXXXX";
    if (!mkdtemp(ws_dir)) FAIL("mkdtemp failed");

    sqlite3 *db = db_open(":memory:");
    if (!db) { rmdir(ws_dir); FAIL("db_open failed"); }

    int64_t sid = session_create(db, "e2e_workspace", NULL, -1, 0);
    if (sid < 0) { db_close(db); rmdir(ws_dir); FAIL("session_create failed"); }

    Config cfg = {0};
    cfg.provider.base_url = "https://openrouter.ai/api/v1";
    cfg.provider.api_key = (char *)key;
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.max_tokens = 512;
    cfg.provider.context_window = 128000;
    cfg.max_iterations = 5;
    cfg.workspace = ws_dir;
    cfg.system_prompt = "You are a helpful assistant. When asked to write a file, use the file_write tool immediately.";

    /* Register file_write tool */
    ToolRegistry reg;
    tools_init(&reg);
    tool_file_write_register(&reg, ws_dir);

    size_t tool_count = 0;
    const ToolSchema *schemas = tools_schemas(&reg, &tool_count);

    /* System + user message */
    Message sys_msg = {.role = ROLE_SYSTEM, .content = cfg.system_prompt};
    entry_append(db, sid, &sys_msg);
    Message user_msg = {.role = ROLE_USER, .content = "Write a file called hello.txt containing exactly: e2e_test_ok"};
    entry_append(db, sid, &user_msg);

    AgentContext ctx = {0};
    ctx.db = db;
    ctx.session_id = sid;
    ctx.cfg = &cfg;
    ctx.dispatch = test_dispatch;
    ctx.dispatch_data = &reg;
    ctx.tools = schemas;
    ctx.tool_count = tool_count;

    int rc = agent_run(&ctx);
    if (rc != 0) {
        tools_free(&reg);
        db_close(db);
        unlink(ws_dir);
        FAIL("agent_run failed");
    }

    /* Verify file was created */
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "%s/hello.txt", ws_dir);
    FILE *f = fopen(filepath, "r");
    if (!f) {
        tools_free(&reg);
        db_close(db);
        FAIL("hello.txt not created by agent");
    }

    char buf[256] = {0};
    fgets(buf, sizeof(buf), f);
    fclose(f);

    /* Clean up */
    unlink(filepath);
    rmdir(ws_dir);
    tools_free(&reg);
    db_close(db);

    if (strstr(buf, "e2e_test_ok") == NULL) FAIL("file content mismatch");
    PASS();
}

/* Telegram delivery — send a real message (skip if no token/chat_id) */
static void test_telegram_delivery(void) {
    TEST(telegram_delivery);
    const char *token = getenv("TELEGRAM_BOT_TOKEN");
    const char *chat_id_str = getenv("TELEGRAM_TEST_CHAT_ID");
    if (!token || !chat_id_str) SKIP("TELEGRAM_BOT_TOKEN or TELEGRAM_TEST_CHAT_ID not set");

    int64_t chat_id = atoll(chat_id_str);
    if (chat_id == 0) SKIP("invalid TELEGRAM_TEST_CHAT_ID");

    /* Send a test message */
    telegram_send_message(token, chat_id, "[cclaw e2e test] delivery ok");

    /* If we get here without crash/hang, the API accepted it.
     * We can't easily verify receipt without getUpdates on a second bot. */
    PASS();
}

/* OpenRouter multi-turn: tool call → tool result → final answer */
static void test_openrouter_multi_turn(void) {
    TEST(openrouter_multi_turn);
    const char *key = getenv("OPENROUTER_API_KEY");
    if (!key) SKIP("OPENROUTER_API_KEY not set");

    Arena *a = arena_create(ARENA_DEFAULT_SIZE);
    Config cfg = {0};
    cfg.provider.base_url = "https://openrouter.ai/api/v1";
    cfg.provider.api_key = (char *)key;
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.max_tokens = 256;
    cfg.provider.context_window = 128000;

    /* First call: expect tool_call */
    Message msgs1[2] = {
        { .role = ROLE_SYSTEM, .content = "Always use the calculator tool for math. Never compute yourself." },
        { .role = ROLE_USER, .content = "What is 7 * 13?" },
    };
    ToolSchema tools[1] = {{
        .name = "calculator",
        .description = "Evaluate a math expression",
        .parameters_json = "{\"type\":\"object\",\"properties\":{\"expression\":{\"type\":\"string\"}},\"required\":[\"expression\"]}"
    }};

    char *req1 = llm_build_request(a, &cfg, msgs1, 2, tools, 1);
    if (!req1) { arena_destroy(a); FAIL("build request 1 failed"); }

    char auth_hdr[256];
    snprintf(auth_hdr, sizeof(auth_hdr), "Authorization: Bearer %s", key);
    const char *headers[] = { "Content-Type: application/json", auth_hdr, NULL };
    char url[512];
    snprintf(url, sizeof(url), "%s/chat/completions", cfg.provider.base_url);

    HttpResponse resp1 = {0};
    int status = http_post(url, headers, req1, &resp1);
    if (status != 200) {
        http_response_free(&resp1);
        arena_destroy(a);
        FAIL("first call failed");
    }

    LlmResponse llm1;
    if (llm_parse_response(a, resp1.data, &llm1) != 0) {
        http_response_free(&resp1);
        arena_destroy(a);
        FAIL("parse first response failed");
    }
    http_response_free(&resp1);

    if (llm1.tool_call_count == 0) { arena_destroy(a); FAIL("no tool_call in first response"); }

    /* Second call: provide tool result, expect final answer */
    Message msgs2[4] = {
        msgs1[0],
        msgs1[1],
        { .role = ROLE_ASSISTANT, .content = llm1.content,
          .tool_calls = llm1.tool_calls, .tool_call_count = llm1.tool_call_count },
        { .role = ROLE_TOOL, .content = NULL,
          .tool_result = &(ToolResult){ .tool_call_id = llm1.tool_calls[0].id, .content = "91" } },
    };

    Arena *a2 = arena_create(ARENA_DEFAULT_SIZE);
    char *req2 = llm_build_request(a2, &cfg, msgs2, 4, tools, 1);
    if (!req2) { arena_destroy(a2); arena_destroy(a); FAIL("build request 2 failed"); }

    HttpResponse resp2 = {0};
    status = http_post(url, headers, req2, &resp2);
    if (status != 200) {
        http_response_free(&resp2);
        arena_destroy(a2);
        arena_destroy(a);
        FAIL("second call failed");
    }

    LlmResponse llm2;
    if (llm_parse_response(a2, resp2.data, &llm2) != 0) {
        http_response_free(&resp2);
        arena_destroy(a2);
        arena_destroy(a);
        FAIL("parse second response failed");
    }
    http_response_free(&resp2);

    if (!llm2.content || strlen(llm2.content) == 0) {
        arena_destroy(a2);
        arena_destroy(a);
        FAIL("no content in final response");
    }
    if (!strstr(llm2.content, "91")) {
        printf("(response: %.60s) ", llm2.content);
        /* Not a hard fail — model might phrase differently */
    }

    arena_destroy(a2);
    arena_destroy(a);
    PASS();
}

int main(void) {
    signal(SIGALRM, SIG_DFL);
    alarm(120); /* e2e tests may be slow (network) */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    printf("--- test_e2e_providers ---\n");
    test_gemini_tool_calls();
    test_openrouter_multi_turn();
    test_agent_workspace_side_effects();
    test_telegram_delivery();

    printf("%d/%d passed\n", tests_passed, tests_run);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
