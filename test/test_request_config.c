/* Unit test for request_config tool (new request_changes dialect) +
 * request_config_changes_apply. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "test_util.h"
#include "tools.h"
#include "tool_request_config.h"
#include "agent_config.h"
#include "config_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── helpers ────────────────────────────────────────────────────────── */

static char *call_handler(ToolRegistry *reg, const char *args) {
    ToolEntry *e = tools_lookup(reg, "request_config");
    assert(e != NULL);
    return e->handler(args, e->user_data);
}

/* Seed a registered config key so the handler allows it. */
static void seed_config_key(sqlite3 *db, const char *key) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO config(key, default_value, description)"
        " VALUES('%s','0','test knob')", key);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

/* ── tests ──────────────────────────────────────────────────────────── */

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
static void test_handler_unavailable(void) {
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, NULL);

    char *result = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}}}");
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    printf("  PASS test_handler_unavailable\n");
}

/* 1. Park a tools grant; verify approvals row and session state. */
static void test_park_tools_grant(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);
    assert(sid > 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "call_1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* awaiting_approval is only reachable from a busy state — in production
     * the dispatcher holds tool_running while the handler parks. */
    assert(session_set_state(db, sid, "tool_running") == 0);
    char *result = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}}}");
    assert(result == NULL); /* parked */

    /* Verify approvals row. */
    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT action, json_extract(args_json,'$.changes.grants.tools[0]')"
        " FROM approvals WHERE session_id=?1 AND tool_name='request_config'",
        -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "request_changes") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "shell_exec") == 0);
    sqlite3_finalize(s);

    /* Verify session state. */
    rc = sqlite3_prepare_v2(db,
        "SELECT state FROM sessions WHERE id=?1", -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "awaiting_approval") == 0);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_park_tools_grant\n");
}


/* 2. Error cases — missing changes, empty doc, unknown section, unknown
 *    grants key, non-array, relative path, unknown config key, provider
 *    errors. */
static void test_error_missing_changes(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "e1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* No 'changes' field at all. */
    char *err = call_handler(&reg, "{\"action\":\"request_changes\"}");
    assert(err != NULL && strstr(err, "changes") != NULL);
    free(err);

    /* Empty changes document (no sections). */
    ctx.current_tool_call_id = "e2";
    err = call_handler(&reg, "{\"action\":\"request_changes\",\"changes\":{}}");
    assert(err != NULL && strstr(err, "nothing to request") != NULL);
    free(err);

    /* Unknown section name. */
    ctx.current_tool_call_id = "e3";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"tokens\":{\"a\":1}}}");
    assert(err != NULL && strstr(err, "unknown changes section") != NULL);
    free(err);

    /* Unknown grants key. */
    ctx.current_tool_call_id = "e4";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"networks\":[\"x\"]}}}");
    assert(err != NULL && strstr(err, "unknown grants key") != NULL);
    free(err);

    /* Grants value not an array. */
    ctx.current_tool_call_id = "e5";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"tools\":\"shell_exec\"}}}");
    assert(err != NULL && strstr(err, "array") != NULL);
    free(err);

    /* Array entry empty string. */
    ctx.current_tool_call_id = "e6";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"tools\":[\"\"]}}}");
    assert(err != NULL && strstr(err, "non-empty") != NULL);
    free(err);

    /* Relative path grant. */
    ctx.current_tool_call_id = "e7";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"read_paths\":[\"relative/dir\"]}}}");
    assert(err != NULL && strstr(err, "absolute") != NULL);
    free(err);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_error_missing_changes\n");
}


static void test_error_config_keys(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "ck1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* Unknown config key. */
    char *err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"config\":{\"no_such_key\":\"1\"}}}");
    assert(err != NULL && strstr(err, "unknown config key") != NULL);
    assert(strstr(err, "search_config") != NULL);
    free(err);

    /* Config value not a string. */
    ctx.current_tool_call_id = "ck2";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"config\":{\"web_port\":123}}}");
    assert(err != NULL && strstr(err, "string") != NULL);
    free(err);

    /* Secret-flagged config key. */
    assert(sqlite3_exec(db,
        "INSERT OR IGNORE INTO config(key, default_value, description, secret)"
        " VALUES('my_secret_key','','a secret',1)", NULL, NULL, NULL) == SQLITE_OK);
    ctx.current_tool_call_id = "ck3";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"config\":{\"my_secret_key\":\"x\"}}}");
    assert(err != NULL && strstr(err, "secret") != NULL);
    free(err);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_error_config_keys\n");
}

static void test_error_provider(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "pv1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* Provider name missing. */
    char *err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"provider\":{}}}");
    assert(err != NULL && strstr(err, "provider") != NULL && strstr(err, "required") != NULL);
    free(err);

    /* Unknown provider without base_url. */
    ctx.current_tool_call_id = "pv2";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"provider\":{\"provider\":\"myllm\"}}}");
    assert(err != NULL && strstr(err, "base_url") != NULL);
    free(err);

    /* Non-http base_url (ftp://). */
    ctx.current_tool_call_id = "pv3";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"provider\":"
        "{\"provider\":\"myllm\",\"base_url\":\"ftp://x.com/v1\"}}}");
    assert(err != NULL && strstr(err, "http") != NULL);
    free(err);

    /* Bad api_key_env — contains lowercase / looks like key material. */
    ctx.current_tool_call_id = "pv4";
    err = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"provider\":"
        "{\"provider\":\"openrouter\",\"api_key_env\":\"sk-or-v1-secret\"}}}");
    assert(err != NULL && strstr(err, "api_key_env") != NULL);
    free(err);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_error_provider\n");
}


/* 3. reason propagates to $.reason in the parked approval. */
static void test_reason_propagates(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "r1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    char *result = call_handler(&reg,
        "{\"action\":\"request_changes\","
        "\"changes\":{\"grants\":{\"hosts\":[\"api.example.com\"]}},"
        "\"reason\":\"need the API\"}");
    assert(result == NULL);

    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.reason')"
        " FROM approvals WHERE session_id=?1 AND tool_name='request_config'",
        -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "need the API") == 0);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_reason_propagates\n");
}

/* 4. Provider defaults fill — openrouter with only the name gets the
 *    three canonical defaults in parked JSON. */
static void test_provider_defaults(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "pd1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    char *result = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"provider\":{\"provider\":\"openrouter\"}}}");
    assert(result == NULL);

    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.changes.provider.base_url'),"
        "       json_extract(args_json,'$.changes.provider.model'),"
        "       json_extract(args_json,'$.changes.provider.api_key_env')"
        " FROM approvals WHERE session_id=?1 AND action='request_changes'"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                 "https://openrouter.ai/api/v1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1),
                 "deepseek/deepseek-v4-flash") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2),
                 "OPENROUTER_API_KEY") == 0);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_provider_defaults\n");
}


/* 5. Dedup: same doc twice while pending => "already sent"; after marking
 *    first denied, the same doc parks again; a different doc parks while
 *    first is pending. */
static void test_dedup(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "dd1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    const char *doc = "{\"action\":\"request_changes\","
        "\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}}}";

    /* First request parks. */
    char *r = call_handler(&reg, doc);
    assert(r == NULL);

    /* Same doc again while pending → error containing "already sent". */
    ctx.current_tool_call_id = "dd2";
    r = call_handler(&reg, doc);
    assert(r != NULL && strstr(r, "already sent") != NULL);
    free(r);

    /* A DIFFERENT doc parks fine while first is pending. */
    ctx.current_tool_call_id = "dd3";
    r = call_handler(&reg,
        "{\"action\":\"request_changes\","
        "\"changes\":{\"grants\":{\"hosts\":[\"example.com\"]}}}");
    assert(r == NULL);

    /* Mark the first approval as denied. */
    assert(sqlite3_exec(db,
        "UPDATE approvals SET state='denied'"
        " WHERE action='request_changes'"
        " AND json_extract(args_json,'$.changes.grants.tools[0]')='shell_exec'",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Same doc, re-requested after denial — must park (not dedup'd). */
    ctx.current_tool_call_id = "dd4";
    r = call_handler(&reg, doc);
    assert(r == NULL);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_dedup\n");
}


/* 6. Batch: one doc with grants+config+provider parks exactly ONE approval;
 *    then call request_config_changes_apply and verify grants, config, and
 *    provider rows landed. */
static void test_batch_apply(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    /* Seed an extension-registered config key for the test. */
    seed_config_key(db, "myext.knob");

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "ba1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    char *r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{"
        "\"grants\":{\"tools\":[\"shell_exec\"],\"hosts\":[\"api.example.com\"],"
        "\"read_paths\":[\"/opt/data\"],\"write_paths\":[\"/tmp/out\"]},"
        "\"config\":{\"myext.knob\":\"42\"},"
        "\"provider\":{\"provider\":\"openrouter\"}}}");
    assert(r == NULL); /* parked */

    /* Exactly one approval row. */
    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT count(*) FROM approvals WHERE session_id=?1",
        -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);

    /* Retrieve the parked args_json and apply it. */
    rc = sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1 LIMIT 1",
        -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *args_json = (const char *)sqlite3_column_text(s, 0);
    assert(args_json != NULL);
    char *args_copy = strdup(args_json);
    sqlite3_finalize(s);

    rc = request_config_changes_apply(db, "test", args_copy, 0);
    assert(rc == 0);
    free(args_copy);

    /* Verify grants exist (agent_caps). */
    AgentCaps caps;
    agent_caps_load(db, "test", &caps);
    assert(caps.tool_count == 1);
    assert(strcmp(caps.tools[0], "shell_exec") == 0);
    assert(caps.host_count == 1);
    assert(strcmp(caps.hosts[0], "api.example.com") == 0);
    assert(caps.read_count == 1);
    assert(strcmp(caps.read_paths[0], "/opt/data") == 0);
    assert(caps.write_count == 1);
    assert(strcmp(caps.write_paths[0], "/tmp/out") == 0);
    agent_caps_free(&caps);

    /* Verify config value landed. */
    rc = sqlite3_prepare_v2(db,
        "SELECT value FROM config WHERE key='myext.knob'", -1, &s, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "42") == 0);
    sqlite3_finalize(s);

    /* Verify providers row exists with filled defaults. */
    rc = sqlite3_prepare_v2(db,
        "SELECT base_url, default_model, api_key_env"
        " FROM providers WHERE name='openrouter'", -1, &s, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                 "https://openrouter.ai/api/v1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1),
                 "deepseek/deepseek-v4-flash") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2),
                 "OPENROUTER_API_KEY") == 0);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_batch_apply\n");
}


/* 6b. agent + routes sections: eager validation and batch apply. */
static void test_agent_routes_sections(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    db_agent_upsert(db, "Other", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    /* A live channel plus a route already owned by another agent. */
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name,path) VALUES('tgext','/x');"
        "INSERT INTO channels(name,extension_name) VALUES('tg','tgext');"
        "INSERT INTO channel_routes(channel_name,channel_id,agent_name)"
        " VALUES('tg','-500','Other');", NULL, NULL, NULL) == SQLITE_OK);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "ar1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);
    assert(session_set_state(db, sid, "tool_running") == 0);

    /* agent section: whitelist, integer bounds, model existence. */
    char *r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":"
        "{\"agent\":{\"sandbox_profile\":\"host\"}}}");
    assert(r && strstr(r, "unknown agent key"));
    free(r);
    r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":"
        "{\"agent\":{\"max_iterations\":0}}}");
    assert(r && strstr(r, "positive integer"));
    free(r);
    r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":"
        "{\"agent\":{\"primary_model\":\"ghost-model\"}}}");
    assert(r && strstr(r, "unknown model"));
    free(r);

    /* routes section: shape, unknown channel, wildcard, foreign owner. */
    r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"routes\":[\"tg\"]}}");
    assert(r && strstr(r, "'channel:chat_id'"));
    free(r);
    r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"routes\":[\"nochan:1\"]}}");
    assert(r && strstr(r, "unknown channel"));
    free(r);
    r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"routes\":[\"tg:*\"]}}");
    assert(r && strstr(r, "operator-only"));
    free(r);
    r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{\"routes\":[\"tg:-500\"]}}");
    assert(r && strstr(r, "already owned"));
    free(r);

    /* One document: define a provider, adopt its model as the agent's own
     * primary (the 'model@provider' id the apply seeds), request a route. */
    r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{"
        "\"provider\":{\"provider\":\"gemini\"},"
        "\"agent\":{\"primary_model\":\"gemini-2.5-flash@gemini\","
        "\"max_iterations\":40},"
        "\"routes\":[\"tg:777\"]}}");
    assert(r == NULL); /* parked */

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *args_copy = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);

    assert(request_config_changes_apply(db, "test", args_copy, 0) == 0);
    free(args_copy);

    /* models row seeded → the adopted id resolves. */
    assert(sqlite3_prepare_v2(db,
        "SELECT provider_name, model FROM models"
        " WHERE id='gemini-2.5-flash@gemini'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "gemini") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "gemini-2.5-flash") == 0);
    sqlite3_finalize(s);

    /* agents row updated, untouched columns kept. */
    assert(sqlite3_prepare_v2(db,
        "SELECT primary_model, max_iterations, shell_timeout FROM agents"
        " WHERE name='test'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "gemini-2.5-flash@gemini") == 0);
    assert(sqlite3_column_int(s, 1) == 40);
    assert(sqlite3_column_int(s, 2) == 30); /* default untouched */
    sqlite3_finalize(s);

    /* route landed for this agent, explicit delivery (send authority only). */
    assert(sqlite3_prepare_v2(db,
        "SELECT agent_name, delivery_mode FROM channel_routes"
        " WHERE channel_name='tg' AND channel_id='777'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "test") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "explicit") == 0);
    sqlite3_finalize(s);

    /* Route captured by another agent between park and apply → the whole
     * document rolls back (savepoint), including the agent section. */
    r = call_handler(&reg,
        "{\"action\":\"request_changes\",\"changes\":{"
        "\"agent\":{\"max_iterations\":99},\"routes\":[\"tg:888\"]}}");
    assert(r == NULL);
    assert(sqlite3_exec(db,
        "INSERT INTO channel_routes(channel_name,channel_id,agent_name)"
        " VALUES('tg','888','Other');", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    args_copy = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(request_config_changes_apply(db, "test", args_copy, 0) == -1);
    free(args_copy);
    assert(sqlite3_prepare_v2(db,
        "SELECT max_iterations FROM agents WHERE name='test'",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 40); /* rolled back, not 99 */
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_agent_routes_sections\n");
}


/* 7. Rollback: hand-craft args_json with an unregistered config key, call
 *    request_config_changes_apply, assert -1 AND none of the doc's grants
 *    landed. */
static void test_apply_rollback(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);

    /* Hand-crafted args_json that has a valid tool grant AND an invalid
     * config key (bypass handler validation by constructing the literal). */
    const char *bad_args =
        "{\"action\":\"request_changes\",\"changes\":{"
        "\"grants\":{\"tools\":[\"web_fetch\"]},"
        "\"config\":{\"nonexistent_key_xyz\":\"boom\"}}}";

    int rc = request_config_changes_apply(db, "test", bad_args, 0);
    assert(rc == -1); /* config_set fails for unregistered key */

    /* Verify the tool grant did NOT land (rollback). */
    AgentCaps caps;
    agent_caps_load(db, "test", &caps);
    assert(caps.tool_count == 0);
    agent_caps_free(&caps);

    db_close(db);
    printf("  PASS test_apply_rollback\n");
}


/* 8. rename_agent tests (kept from old suite). */
static void test_rename_agent(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "rn1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* Missing name field. */
    char *err = call_handler(&reg, "{\"action\":\"rename_agent\"}");
    assert(err != NULL && strstr(err, "name") != NULL);
    free(err);

    /* Invalid name (not PascalCase). */
    ctx.current_tool_call_id = "rn2";
    err = call_handler(&reg, "{\"action\":\"rename_agent\",\"name\":\"bad_name\"}");
    assert(err != NULL && strstr(err, "PascalCase") != NULL);
    free(err);

    /* Valid rename parks. */
    ctx.current_tool_call_id = "rn3";
    char *r = call_handler(&reg,
        "{\"action\":\"rename_agent\",\"name\":\"NewAgent\",\"preamble\":\"You are a helper.\","
        "\"reason\":\"better name\"}");
    assert(r == NULL);

    /* Verify parked approval. */
    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.name'),"
        "       json_extract(args_json,'$.preamble'),"
        "       json_extract(args_json,'$.reason')"
        " FROM approvals WHERE session_id=?1 AND action='rename_agent'",
        -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "NewAgent") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "You are a helper.") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "better name") == 0);
    sqlite3_finalize(s);

    /* Dedup: same rename while pending. */
    ctx.current_tool_call_id = "rn4";
    err = call_handler(&reg,
        "{\"action\":\"rename_agent\",\"name\":\"NewAgent\"}");
    assert(err != NULL && strstr(err, "already sent") != NULL);
    free(err);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_rename_agent\n");
}

/* Unknown action value. */
static void test_unknown_action(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "ua1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    char *err = call_handler(&reg, "{\"action\":\"grant_tool\",\"tool\":\"x\"}");
    assert(err != NULL && strstr(err, "request_changes") != NULL);
    free(err);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_unknown_action\n");
}

/* Direct agent_config_grant still works (low-level API). */
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


int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_request_config:\n");
    test_register();
    test_handler_unavailable();
    test_park_tools_grant();
    test_error_missing_changes();
    test_error_config_keys();
    test_error_provider();
    test_reason_propagates();
    test_provider_defaults();
    test_dedup();
    test_batch_apply();
    test_agent_routes_sections();
    test_apply_rollback();
    test_rename_agent();
    test_unknown_action();
    test_add_tool_to_config();
    printf("\nAll request_config tests passed.\n");
    return 0;
}
