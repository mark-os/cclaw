#define _GNU_SOURCE
#include "tool_cron.h"
#include "cron.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_cron_set_valid(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "test_agent"};
    char *result = tool_cron_set_handler(
        "{\"name\":\"daily\",\"cron_expr\":\"0 9 * * *\",\"task\":\"good morning\"}",
        &ctx);
    assert(result);
    assert(strstr(result, "created cron job id="));
    assert(strstr(result, "daily"));
    free(result);
    db_close(db);
    printf("  PASS: cron_set valid\n");
}

static void test_cron_set_invalid_expr(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "test_agent"};
    char *result = tool_cron_set_handler(
        "{\"name\":\"bad\",\"cron_expr\":\"invalid\",\"task\":\"x\"}", &ctx);
    assert(result);
    assert(strstr(result, "error"));
    free(result);
    db_close(db);
    printf("  PASS: cron_set invalid expr\n");
}

static void test_cron_set_missing_fields(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "test_agent"};
    char *result = tool_cron_set_handler("{\"name\":\"x\"}", &ctx);
    assert(result);
    assert(strstr(result, "error"));
    free(result);
    db_close(db);
    printf("  PASS: cron_set missing fields\n");
}

static void test_cron_list_empty(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "test_agent"};
    char *result = tool_cron_list_handler("{}", &ctx);
    assert(result);
    assert(strcmp(result, "no cron jobs") == 0);
    free(result);
    db_close(db);
    printf("  PASS: cron_list empty\n");
}

static void test_cron_list_with_jobs(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    cron_add(db, "test_agent", "j1", "0 * * * *", 0, 0, sid, "task1");
    cron_add(db, "test_agent", "j2", "30 2 * * *", 0, 0, sid, "task2");

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "test_agent"};
    char *result = tool_cron_list_handler("{}", &ctx);
    assert(result);
    assert(strstr(result, "j1"));
    assert(strstr(result, "j2"));
    assert(strstr(result, "task1"));
    free(result);
    db_close(db);
    printf("  PASS: cron_list with jobs\n");
}

static void test_cron_remove_valid(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    int64_t jid = cron_add(db, "test_agent", "rm_me", "0 * * * *", 0, 0, sid, "bye");
    assert(jid > 0);

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "test_agent"};
    char buf[64];
    snprintf(buf, sizeof(buf), "{\"id\":%lld}", (long long)jid);
    char *result = tool_cron_remove_handler(buf, &ctx);
    assert(result);
    assert(strstr(result, "removed cron job"));
    free(result);

    /* Verify it's gone */
    int count = 0;
    CronJob *jobs = cron_list(db, "test_agent", &count);
    assert(count == 0 && jobs == NULL);

    db_close(db);
    printf("  PASS: cron_remove valid\n");
}

static void test_cron_remove_nonexistent(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "test_agent"};
    char *result = tool_cron_remove_handler("{\"id\":999}", &ctx);
    assert(result);
    assert(strstr(result, "error"));
    free(result);
    db_close(db);
    printf("  PASS: cron_remove nonexistent\n");
}

static void test_cron_register(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    ToolRegistry reg;
    tools_init(&reg);
    ToolCronCtx ctx = {.db = db, .session_id = 1, .agent_name = "test"};
    assert(tool_cron_register(&reg, &ctx) == 0);
    assert(tools_lookup(&reg, "cron_set") != NULL);
    assert(tools_lookup(&reg, "cron_list") != NULL);
    assert(tools_lookup(&reg, "cron_remove") != NULL);
    tools_free(&reg);
    db_close(db);
    printf("  PASS: tool registration\n");
}

int main(void) {
    printf("test_tool_cron:\n");
    test_cron_set_valid();
    test_cron_set_invalid_expr();
    test_cron_set_missing_fields();
    test_cron_list_empty();
    test_cron_list_with_jobs();
    test_cron_remove_valid();
    test_cron_remove_nonexistent();
    test_cron_register();
    printf("ALL PASSED\n");
    return 0;
}
