#include "db.h"
#include "llm_payload.h"
#include "config.h"
#include "context.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *DB_PATH = "/tmp/test_cclaw_payload.db";

static void setup_session(sqlite3 *db, int64_t *sid) {
    *sid = session_create(db, "test", "default", -1, 0);
    assert(*sid > 0);

    /* System message */
    Message sys = {.role = ROLE_SYSTEM, .content = "You are helpful."};
    entry_append(db, *sid, &sys);

    /* User message with special chars */
    Message user = {.role = ROLE_USER, .content = "Hello \"world\"\nLine2\ttab"};
    entry_append(db, *sid, &user);

    /* Assistant reply */
    Message asst = {.role = ROLE_ASSISTANT, .content = "Hi there!"};
    entry_append(db, *sid, &asst);
}

static void test_openai_payload(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.max_tokens = 1024;
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.provider.context_window = 128000;
    cfg.stream = 1;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);
    assert(plan.count >= 3);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, NULL, &payload) == 0);
    assert(payload.body);

    /* Verify valid JSON */
    cJSON *root = cJSON_Parse(payload.body);
    assert(root);

    /* Check model */
    cJSON *model = cJSON_GetObjectItem(root, "model");
    assert(model && strcmp(model->valuestring, "test-model") == 0);

    /* Check messages array */
    cJSON *msgs = cJSON_GetObjectItem(root, "messages");
    assert(msgs && cJSON_IsArray(msgs));
    int mcount = cJSON_GetArraySize(msgs);
    assert(mcount >= 3);

    /* First should be system */
    cJSON *m0 = cJSON_GetArrayItem(msgs, 0);
    cJSON *r0 = cJSON_GetObjectItem(m0, "role");
    assert(r0 && strcmp(r0->valuestring, "system") == 0);

    /* Second should be user with escaped content */
    cJSON *m1 = cJSON_GetArrayItem(msgs, 1);
    cJSON *c1 = cJSON_GetObjectItem(m1, "content");
    assert(c1 && strstr(c1->valuestring, "Hello \"world\""));
    assert(strstr(c1->valuestring, "\n")); /* newline preserved */

    /* Check stream flag */
    cJSON *stream = cJSON_GetObjectItem(root, "stream");
    assert(stream && cJSON_IsTrue(stream));

    /* Check max_tokens */
    cJSON *mt = cJSON_GetObjectItem(root, "max_tokens");
    assert(mt && mt->valueint == 1024);

    cJSON_Delete(root);
    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_openai_payload\n");
}

static void test_gemini_payload(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    Config cfg = {0};
    cfg.provider.model = "gemini-2.5-flash";
    cfg.provider.max_tokens = 2048;
    cfg.provider.endpoint_type = ENDPOINT_GEMINI;
    cfg.provider.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, NULL, &payload) == 0);
    assert(payload.body);

    cJSON *root = cJSON_Parse(payload.body);
    assert(root);

    /* Should have systemInstruction */
    cJSON *si = cJSON_GetObjectItem(root, "systemInstruction");
    assert(si);
    cJSON *parts = cJSON_GetObjectItem(si, "parts");
    assert(parts && cJSON_GetArraySize(parts) > 0);

    /* Should have contents array (no system entries in it) */
    cJSON *contents = cJSON_GetObjectItem(root, "contents");
    assert(contents && cJSON_IsArray(contents));
    /* Verify no system role in contents */
    cJSON *c; int found_sys = 0;
    cJSON_ArrayForEach(c, contents) {
        cJSON *r = cJSON_GetObjectItem(c, "role");
        if (r && strcmp(r->valuestring, "system") == 0) found_sys = 1;
    }
    assert(!found_sys);

    cJSON_Delete(root);
    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_gemini_payload\n");
}

static void test_payload_with_tools(void) {
    unlink(DB_PATH);
    sqlite3 *db = db_open(DB_PATH);
    assert(db);

    int64_t sid;
    setup_session(db, &sid);

    /* Seed a tool in the tools table */
    sqlite3_exec(db,
        "INSERT INTO tools(name,description,parameters_json) VALUES("
        "'file_read','Read a file',"
        "'{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}}}')",
        NULL, NULL, NULL);

    Config cfg = {0};
    cfg.provider.model = "test-model";
    cfg.provider.endpoint_type = ENDPOINT_OPENAI;
    cfg.provider.context_window = 128000;

    ContextPlan plan = {0};
    assert(context_plan(db, sid, &cfg, 0, &plan) == 0);

    LlmPayload payload;
    assert(llm_build_payload(db, sid, &cfg, &plan, NULL, NULL, &payload) == 0);
    assert(payload.body);

    cJSON *root = cJSON_Parse(payload.body);
    assert(root);

    cJSON *tarr = cJSON_GetObjectItem(root, "tools");
    assert(tarr && cJSON_IsArray(tarr));
    assert(cJSON_GetArraySize(tarr) == 1);
    cJSON *t0 = cJSON_GetArrayItem(tarr, 0);
    cJSON *fn = cJSON_GetObjectItem(t0, "function");
    assert(fn);
    cJSON *fname = cJSON_GetObjectItem(fn, "name");
    assert(fname && strcmp(fname->valuestring, "file_read") == 0);

    cJSON_Delete(root);
    llm_payload_release(&payload);
    context_plan_free(&plan);
    db_close(db); unlink(DB_PATH);
    printf("  PASS test_payload_with_tools\n");
}

int main(void) {
    printf("test_llm_payload:\n");
    test_openai_payload();
    test_gemini_payload();
    test_payload_with_tools();
    printf("All payload tests passed.\n");
    return 0;
}
