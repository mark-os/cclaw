#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "llm.h"
#include "http.h"
#include "db.h"
#include "db_response.h"
#include "test_util.h"
#include <sqlite3.h>
#include <curl/curl.h>

static sqlite3 *g_db;
static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define SKIP(msg) do { tests_passed++; printf("SKIP: %s\n", msg); return; } while(0)

static const char *get_api_key(void) {
    return getenv("OPENROUTER_API_KEY");
}

/* Build request JSON using SQLite json_object */
static char *build_request(sqlite3 *db, const char *model, int max_tokens,
                           const Message *msgs, size_t msg_count,
                           const ToolSchema *tools, size_t tool_count) {
    /* Build messages array via a temp table (one row per message) */
    sqlite3_exec(db, "DROP TABLE IF EXISTS _tmsg;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "CREATE TEMP TABLE _tmsg(pos INTEGER PRIMARY KEY, role TEXT, content TEXT);",
        NULL, NULL, NULL);

    sqlite3_stmt *ins;
    sqlite3_prepare_v2(db, "INSERT INTO _tmsg VALUES(?,?,?)", -1, &ins, NULL);
    for (size_t i = 0; i < msg_count; i++) {
        const char *role = msgs[i].role == ROLE_SYSTEM ? "system" :
                           msgs[i].role == ROLE_USER ? "user" :
                           msgs[i].role == ROLE_ASSISTANT ? "assistant" : "tool";
        sqlite3_bind_int(ins, 1, (int)i);
        sqlite3_bind_text(ins, 2, role, -1, SQLITE_STATIC);
        sqlite3_bind_text(ins, 3, msgs[i].content ? msgs[i].content : "", -1, SQLITE_STATIC);
        sqlite3_step(ins);
        sqlite3_reset(ins);
    }
    sqlite3_finalize(ins);

    /* Build tools array if needed */
    char *tools_json = NULL;
    if (tools && tool_count > 0) {
        sqlite3_exec(db, "DROP TABLE IF EXISTS _ttool;", NULL, NULL, NULL);
        sqlite3_exec(db,
            "CREATE TEMP TABLE _ttool(pos INTEGER PRIMARY KEY, name TEXT, description TEXT, params TEXT);",
            NULL, NULL, NULL);
        sqlite3_stmt *tins;
        sqlite3_prepare_v2(db, "INSERT INTO _ttool VALUES(?,?,?,?)", -1, &tins, NULL);
        for (size_t i = 0; i < tool_count; i++) {
            sqlite3_bind_int(tins, 1, (int)i);
            sqlite3_bind_text(tins, 2, tools[i].name, -1, SQLITE_STATIC);
            sqlite3_bind_text(tins, 3, tools[i].description ? tools[i].description : "", -1, SQLITE_STATIC);
            sqlite3_bind_text(tins, 4, tools[i].parameters_json ? tools[i].parameters_json : "{}", -1, SQLITE_STATIC);
            sqlite3_step(tins);
            sqlite3_reset(tins);
        }
        sqlite3_finalize(tins);

        sqlite3_stmt *ts;
        sqlite3_prepare_v2(db,
            "SELECT json_group_array(json_object('type','function',"
            " 'function',json_object('name',name,'description',description,"
            " 'parameters',json(params)))) FROM _ttool ORDER BY pos",
            -1, &ts, NULL);
        if (sqlite3_step(ts) == SQLITE_ROW) {
            const char *t = (const char *)sqlite3_column_text(ts, 0);
            if (t) tools_json = strdup(t);
        }
        sqlite3_finalize(ts);
    }

    /* Assemble full request */
    const char *sql = tools_json
        ? "SELECT json_object('model',?1,'max_tokens',?2,"
          " 'messages',(SELECT json_group_array(json_object('role',role,'content',content)) FROM _tmsg ORDER BY pos),"
          " 'tools',json(?3))"
        : "SELECT json_object('model',?1,'max_tokens',?2,"
          " 'messages',(SELECT json_group_array(json_object('role',role,'content',content)) FROM _tmsg ORDER BY pos))";

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, model, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, max_tokens);
    if (tools_json) sqlite3_bind_text(stmt, 3, tools_json, -1, SQLITE_STATIC);

    char *result = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(stmt, 0);
        if (t) result = strdup(t);
    }
    sqlite3_finalize(stmt);
    free(tools_json);
    return result;
}

/* Call LLM, ingest into a fresh session. Returns session id (>0) or -1. */
static int64_t call_llm(const Config *cfg, const Message *msgs,
                        size_t msg_count, const ToolSchema *tools,
                        size_t tool_count, TypedIngestResult *out) {
    char *req_json = build_request(g_db, cfg->provider.model,
                                   cfg->provider.max_tokens,
                                   msgs, msg_count, tools, tool_count);
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

    int64_t sid = session_create(g_db, "e2e", NULL, -1, 0);
    int64_t turn = db_next_turn_id(g_db, sid);
    LlmRespStatus st = db_ingest_response(g_db, sid, turn, cfg->provider.model,
                                          ENDPOINT_OPENAI, resp.data, out);
    http_response_free(&resp);
    return (st == LLM_RESP_OK) ? sid : -1;
}

/* Fetch the assistant_message content for a session (caller frees). */
static char *asst_content(int64_t sid) {
    sqlite3_stmt *s;
    sqlite3_prepare_v2(g_db,
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

/* live call with simple prompt, verify content response */
static void test_live_content_response(void) {
    TEST(live_content_response);
    const char *key = get_api_key();
    if (!key) SKIP("OPENROUTER_API_KEY not set");

    Config cfg = {0};
    cfg.provider.base_url = "https://openrouter.ai/api/v1";
    cfg.provider.api_key = (char *)key;
    cfg.provider.model = "deepseek/deepseek-v4-flash";
    cfg.provider.max_tokens = 64;
    cfg.provider.context_window = 128000;

    Message msgs[1] = {{ .role = ROLE_USER, .content = "Reply with exactly: hello" }};

    TypedIngestResult ir;
    int64_t sid = call_llm(&cfg, msgs, 1, NULL, 0, &ir);
    if (sid < 0) FAIL("LLM call failed");
    char *c = asst_content(sid);
    if (!c || strlen(c) == 0) { free(c); FAIL("empty content"); }
    free(c);
    if (ir.prompt_tokens == 0 && ir.completion_tokens == 0) FAIL("no usage reported");

    PASS();
}

/* live call with tool, verify tool_calls round-trip */
static void test_live_tool_call_roundtrip(void) {
    TEST(live_tool_call_roundtrip);
    const char *key = get_api_key();
    if (!key) SKIP("OPENROUTER_API_KEY not set");

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

    TypedIngestResult ir;
    int64_t sid = call_llm(&cfg, msgs, 2, tools, 1, &ir);
    if (sid < 0) FAIL("LLM call failed");

    sqlite3_stmt *s;
    sqlite3_prepare_v2(g_db,
        "SELECT tc.call_id, tc.name, e.content FROM tool_calls tc"
        " JOIN entries e ON e.id=tc.entry_id WHERE tc.session_id=? ORDER BY tc.rowid LIMIT 1;",
        -1, &s, NULL);
    sqlite3_bind_int64(s, 1, sid);
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_finalize(s); FAIL("no tool_calls returned"); }
    const char *id = (const char *)sqlite3_column_text(s, 0);
    const char *name = (const char *)sqlite3_column_text(s, 1);
    const char *args = (const char *)sqlite3_column_text(s, 2);
    if (!id || strlen(id) == 0) { sqlite3_finalize(s); FAIL("empty tool_call id"); }
    if (!name || strcmp(name, "get_weather") != 0) { sqlite3_finalize(s); FAIL("wrong tool name"); }
    if (!args || strlen(args) == 0) { sqlite3_finalize(s); FAIL("empty arguments"); }
    sqlite3_finalize(s);

    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_db = test_db_open(":memory:");
    printf("--- test_integration_llm ---\n");
    test_live_content_response();
    test_live_tool_call_roundtrip();
    printf("%d/%d passed\n", tests_passed, tests_run);
    sqlite3_close(g_db);
    curl_global_cleanup();
    return tests_passed == tests_run ? 0 : 1;
}
