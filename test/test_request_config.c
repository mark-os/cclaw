/* T274: Unit test for request_config tool + agent_config_grant */
#include "db.h"
#include "test_util.h"
#include "tools.h"
#include "tool_request_config.h"
#include "agent_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_request_config_register(&reg, NULL);
    assert(rc == 0);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    assert(e != NULL);
    assert(strcmp(e->name, "request_config") == 0);
    assert(e->handler != NULL);

    tools_free(&reg);
    printf("  PASS test_register\n");
}

/* With NULL context, handler returns unavailable error. */
static void test_handler_returns_error(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, NULL);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    char *result = e->handler("{\"action\":\"grant_tool\",\"tool\":\"shell_exec\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    printf("  PASS test_handler_returns_error\n");
}

static void test_missing_tool_field(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, NULL);

    ToolEntry *e = tools_lookup(&reg, "request_config");
    char *result = e->handler("{\"action\":\"grant_tool\"}", e->user_data);
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    printf("  PASS test_missing_tool_field\n");
}

static void test_add_tool_to_config(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    db_agent_upsert(db, "test", NULL, NULL);

    int rc = agent_config_grant(db, "test", "tool", "shell_exec", 0);
    assert(rc == 0);

    AgentCaps caps;
    agent_caps_load(db, "test", &caps);
    assert(caps.tool_count == 1);
    agent_caps_free(&caps);

    db_close(db);
    printf("  PASS test_add_tool_to_config\n");
}

/* Helper: invoke the registered handler directly. */
static char *call_handler(ToolRegistry *reg, const char *args) {
    ToolEntry *e = tools_lookup(reg, "request_config");
    assert(e != NULL);
    return e->handler(args, e->user_data);
}

static void test_reason_in_args_json(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);
    assert(sid > 0);

    RequestConfigCtx ctx = {
        .db = db,
        .agent_name = "test",
        .session_id = sid,
        .agents_dir = NULL,
        .current_tool_call_id = "call_1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    char *result = call_handler(&reg,
        "{\"action\":\"grant_host\",\"host\":\"api.example.com\",\"reason\":\"need the API\"}");
    assert(result == NULL); /* parked */

    /* Verify the approvals row contains the reason and host. */
    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1 AND tool_name='request_config'",
        -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *aj = (const char *)sqlite3_column_text(s, 0);
    assert(aj != NULL);
    assert(strstr(aj, "\"reason\":\"need the API\"") != NULL);
    assert(strstr(aj, "\"host\":\"api.example.com\"") != NULL);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_reason_in_args_json\n");
}

static void test_grant_path_default_mode_read(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);
    assert(sid > 0);

    RequestConfigCtx ctx = {
        .db = db,
        .agent_name = "test",
        .session_id = sid,
        .agents_dir = NULL,
        .current_tool_call_id = "call_2"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* Default mode (no mode supplied) should become "read". */
    char *result = call_handler(&reg,
        "{\"action\":\"grant_path\",\"path\":\"/opt/data\"}");
    assert(result == NULL); /* parked */

    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.mode'), json_extract(args_json,'$.path')"
        " FROM approvals WHERE session_id=?1 AND tool_name='request_config'"
        " ORDER BY id DESC LIMIT 1",
        -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *mode = (const char *)sqlite3_column_text(s, 0);
    const char *path = (const char *)sqlite3_column_text(s, 1);
    assert(mode != NULL && strcmp(mode, "read") == 0);
    assert(path != NULL && strcmp(path, "/opt/data") == 0);
    sqlite3_finalize(s);

    /* Invalid mode returns an error mentioning "mode". */
    ctx.current_tool_call_id = "call_3";
    char *err = call_handler(&reg,
        "{\"action\":\"grant_path\",\"path\":\"/opt/data\",\"mode\":\"rw\"}");
    assert(err != NULL);
    assert(strstr(err, "mode") != NULL);
    free(err);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_grant_path_default_mode_read\n");
}

static void test_denied_dedup(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);
    assert(sid > 0);

    /* Pre-insert a denied approval for grant_host / blocked.example.com. */
    sqlite3_stmt *ins;
    int rc = sqlite3_prepare_v2(db,
        "INSERT INTO approvals (session_id, tool_call_id, tool_name, action, args_json, resolve, state)"
        " VALUES (?1,'call_0','request_config','grant_host',"
        "'{\"action\":\"grant_host\",\"host\":\"blocked.example.com\"}','apply','denied')",
        -1, &ins, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(ins, 1, sid);
    assert(sqlite3_step(ins) == SQLITE_DONE);
    sqlite3_finalize(ins);

    RequestConfigCtx ctx = {
        .db = db,
        .agent_name = "test",
        .session_id = sid,
        .agents_dir = NULL,
        .current_tool_call_id = "call_4"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* Same host (different reason) — should hit dedup and return error. */
    char *result = call_handler(&reg,
        "{\"action\":\"grant_host\",\"host\":\"blocked.example.com\",\"reason\":\"different reason\"}");
    assert(result != NULL);
    assert(strstr(result, "already denied in this session") != NULL);
    free(result);

    /* Verify no new row was created — count should still be 1. */
    sqlite3_stmt *cnt;
    rc = sqlite3_prepare_v2(db,
        "SELECT count(*) FROM approvals WHERE session_id=?1",
        -1, &cnt, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(cnt, 1, sid);
    assert(sqlite3_step(cnt) == SQLITE_ROW);
    assert(sqlite3_column_int(cnt, 0) == 1);
    sqlite3_finalize(cnt);

    /* Different host should park successfully (dedup must not overmatch). */
    ctx.current_tool_call_id = "call_5";
    char *ok = call_handler(&reg,
        "{\"action\":\"grant_host\",\"host\":\"other.example.com\"}");
    assert(ok == NULL); /* parked */

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_denied_dedup\n");
}

int main(void) {
    printf("test_request_config (T274):\n");
    test_register();
    test_handler_returns_error();
    test_missing_tool_field();
    test_add_tool_to_config();
    test_reason_in_args_json();
    test_grant_path_default_mode_read();
    test_denied_dedup();
    printf("\nAll request_config tests passed.\n");
    return 0;
}
