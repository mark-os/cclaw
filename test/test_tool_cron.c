#define _GNU_SOURCE
#include "tool_cron.h"
#include "config_registry.h"
#include "cron.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

static void test_cron_set_oneshot_valid(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", "A", -1, 0);
    int64_t now = (int64_t)time(NULL);

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "A"};
    char *result = tool_cron_set_handler(
        "{\"name\":\"later\",\"in_seconds\":3600,\"task\":\"check build\"}", &ctx);
    assert(result && strstr(result, "created cron job id="));
    free(result);

    /* run_at translated to now + in_seconds; recorded as a one-shot. */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT run_at, kind, cron_expr FROM cron_jobs WHERE session_id=?"
        " ORDER BY id DESC LIMIT 1", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_int64(st, 1, sid);
    assert(sqlite3_step(st) == SQLITE_ROW);
    int64_t run_at = sqlite3_column_int64(st, 0);
    const char *kind = (const char *)sqlite3_column_text(st, 1);
    const char *expr = (const char *)sqlite3_column_text(st, 2);
    assert(run_at >= now + 3600 - 2 && run_at <= now + 3600 + 2);
    assert(strcmp(kind, "task") == 0);
    assert(expr && expr[0] == '\0');
    sqlite3_finalize(st);

    db_close(db);
    printf("  PASS: cron_set one-shot (now+in_seconds)\n");
}

static void test_cron_set_neither_schedule(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "A"};
    char *result = tool_cron_set_handler("{\"name\":\"x\",\"task\":\"t\"}", &ctx);
    assert(result && strstr(result, "exactly one"));
    free(result);
    db_close(db);
    printf("  PASS: cron_set neither schedule\n");
}

static void test_cron_set_both_schedules(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "A"};
    char *result = tool_cron_set_handler(
        "{\"name\":\"x\",\"cron_expr\":\"0 * * * *\",\"in_seconds\":3600,\"task\":\"t\"}",
        &ctx);
    assert(result && strstr(result, "exactly one"));
    free(result);
    db_close(db);
    printf("  PASS: cron_set both schedules rejected\n");
}

static void test_cron_set_negative_seconds(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", "A", -1, 0);
    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "A"};
    char *result = tool_cron_set_handler(
        "{\"name\":\"x\",\"in_seconds\":-5,\"task\":\"t\"}", &ctx);
    assert(result && strstr(result, "positive"));
    free(result);
    db_close(db);
    printf("  PASS: cron_set negative in_seconds\n");
}

static void test_cron_set_per_session_cap(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", "A", -1, 0);
    assert(config_set(db, "cron_max_jobs_per_session", "1") == 0);

    ToolCronCtx ctx = {.db = db, .session_id = sid, .agent_name = "A"};
    char *r1 = tool_cron_set_handler(
        "{\"name\":\"a\",\"cron_expr\":\"0 * * * *\",\"task\":\"t\"}", &ctx);
    assert(r1 && strstr(r1, "created"));
    free(r1);

    char *r2 = tool_cron_set_handler(
        "{\"name\":\"b\",\"cron_expr\":\"0 * * * *\",\"task\":\"t\"}", &ctx);
    assert(r2 && strstr(r2, "limit reached"));
    free(r2);

    db_close(db);
    printf("  PASS: cron_set per-session cap\n");
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
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_tool_cron:\n");
    test_cron_set_valid();
    test_cron_set_invalid_expr();
    test_cron_set_missing_fields();
    test_cron_set_oneshot_valid();
    test_cron_set_neither_schedule();
    test_cron_set_both_schedules();
    test_cron_set_negative_seconds();
    test_cron_set_per_session_cap();
    test_cron_list_empty();
    test_cron_list_with_jobs();
    test_cron_remove_valid();
    test_cron_remove_nonexistent();
    test_cron_register();
    printf("ALL PASSED\n");
    return 0;
}
