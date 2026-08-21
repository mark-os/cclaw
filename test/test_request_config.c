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
    return e->handler(args, e->user_data, &(int){0});
}

/* Make a credential name resolve, so the config-time key check passes. The
 * value is never read here — validation tests existence only. */
static void seed_key(sqlite3 *db, const char *name) {
    char sql[256];
    snprintf(sql, sizeof(sql),
        "INSERT OR IGNORE INTO secrets(name, value, scope)"
        " VALUES('%s','x','system')", name);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

/* Approve-and-apply the newest parked document for this session. */
static void apply_latest(sqlite3 *db, int64_t sid, char **receipt_out) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *args = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(request_config_changes_apply(db, "test", args, 0, 0, receipt_out) == 0);
    free(args);
}

/* 1 iff sql yields at least one row. */
static int row_exists(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return 0;
    int hit = sqlite3_step(s) == SQLITE_ROW;
    sqlite3_finalize(s);
    return hit;
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
        "{\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}}}");
    assert(result != NULL);
    assert(strstr(result, "error") != NULL);
    free(result);

    tools_free(&reg);
    printf("  PASS test_handler_unavailable\n");
}

/* 1. Park a tools grant; verify approvals row and session state. */
static void test_park_tools_grant(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
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
        "{\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}}}");
    assert(result == NULL); /* parked */

    /* Verify approvals row. */
    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT park_reason, json_extract(args_json,'$.changes.grants.tools[0]')"
        " FROM approvals WHERE session_id=?1 AND tool_name='request_config'",
        -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "approval_required") == 0);
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
    sqlite3 *db = test_db_open_seeded(":memory:");
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
    char *err = call_handler(&reg, "{}");
    assert(err != NULL && strstr(err, "changes") != NULL);
    free(err);

    /* Empty changes document (no sections). */
    ctx.current_tool_call_id = "e2";
    err = call_handler(&reg, "{\"changes\":{}}");
    assert(err != NULL && strstr(err, "nothing to request") != NULL);
    free(err);

    /* Unknown section name. */
    ctx.current_tool_call_id = "e3";
    err = call_handler(&reg,
        "{\"changes\":{\"tokens\":{\"a\":1}}}");
    assert(err != NULL && strstr(err, "unknown changes section") != NULL);
    free(err);

    /* Unknown grants key. */
    ctx.current_tool_call_id = "e4";
    err = call_handler(&reg,
        "{\"changes\":{\"grants\":{\"networks\":[\"x\"]}}}");
    assert(err != NULL && strstr(err, "unknown grants key") != NULL);
    free(err);

    /* Grants value not an array. */
    ctx.current_tool_call_id = "e5";
    err = call_handler(&reg,
        "{\"changes\":{\"grants\":{\"tools\":\"shell_exec\"}}}");
    assert(err != NULL && strstr(err, "array") != NULL);
    free(err);

    /* Array entry empty string. */
    ctx.current_tool_call_id = "e6";
    err = call_handler(&reg,
        "{\"changes\":{\"grants\":{\"tools\":[\"\"]}}}");
    assert(err != NULL && strstr(err, "non-empty") != NULL);
    free(err);

    /* Relative path grant. */
    ctx.current_tool_call_id = "e7";
    err = call_handler(&reg,
        "{\"changes\":{\"grants\":{\"read_paths\":[\"relative/dir\"]}}}");
    assert(err != NULL && strstr(err, "absolute") != NULL);
    free(err);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_error_missing_changes\n");
}


static void test_error_config_keys(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
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
        "{\"changes\":{\"config\":{\"no_such_key\":\"1\"}}}");
    assert(err != NULL && strstr(err, "unknown config key") != NULL);
    assert(strstr(err, "search_config") != NULL);
    free(err);

    /* Config value not a string. */
    ctx.current_tool_call_id = "ck2";
    err = call_handler(&reg,
        "{\"changes\":{\"config\":{\"web_port\":123}}}");
    assert(err != NULL && strstr(err, "string") != NULL);
    free(err);

    /* Secret-flagged config key. */
    assert(sqlite3_exec(db,
        "INSERT OR IGNORE INTO config(key, default_value, description, secret)"
        " VALUES('my_secret_key','','a secret',1)", NULL, NULL, NULL) == SQLITE_OK);
    ctx.current_tool_call_id = "ck3";
    err = call_handler(&reg,
        "{\"changes\":{\"config\":{\"my_secret_key\":\"x\"}}}");
    assert(err != NULL && strstr(err, "secret") != NULL);
    free(err);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_error_config_keys\n");
}

static void test_error_provider(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
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
        "{\"changes\":{\"provider\":{}}}");
    assert(err != NULL && strstr(err, "provider") != NULL && strstr(err, "required") != NULL);
    free(err);

    /* Unknown provider without base_url. */
    ctx.current_tool_call_id = "pv2";
    err = call_handler(&reg,
        "{\"changes\":{\"provider\":{\"provider\":\"myllm\"}}}");
    assert(err != NULL && strstr(err, "base_url") != NULL);
    free(err);

    /* Non-http base_url (ftp://). */
    ctx.current_tool_call_id = "pv3";
    err = call_handler(&reg,
        "{\"changes\":{\"provider\":"
        "{\"provider\":\"myllm\",\"base_url\":\"ftp://x.com/v1\"}}}");
    assert(err != NULL && strstr(err, "http") != NULL);
    free(err);

    /* Bad api_key_env — contains lowercase / looks like key material. */
    ctx.current_tool_call_id = "pv4";
    err = call_handler(&reg,
        "{\"changes\":{\"provider\":"
        "{\"provider\":\"openrouter\",\"api_key_env\":\"sk-or-v1-secret\"}}}");
    assert(err != NULL && strstr(err, "api_key_env") != NULL);
    free(err);

    /* A model belongs in the models section — the provider doc is transport.
     * (Registering a model by re-submitting a provider doc is the trap that
     * took prod's gateway down.) */
    ctx.current_tool_call_id = "pv5";
    err = call_handler(&reg,
        "{\"changes\":{\"provider\":"
        "{\"provider\":\"openrouter\",\"model\":\"some/model\"}}}");
    assert(err != NULL && strstr(err, "models") != NULL);
    free(err);

    /* Config-time key check: the credential this provider would use exists
     * neither in env nor as a system secret. Refuse now, while the agent can
     * still fix it — not silently at request time (A8). */
    ctx.current_tool_call_id = "pv6";
    err = call_handler(&reg,
        "{\"changes\":{\"provider\":"
        "{\"provider\":\"nokey\",\"base_url\":\"https://n.example/v1\"}}}");
    assert(err != NULL && strstr(err, "NOKEY_API_KEY") != NULL
                       && strstr(err, "save_secret") != NULL);
    free(err);

    /* Keyless is legal, but it has to be said out loud. */
    ctx.current_tool_call_id = "pv7";
    char *ok = call_handler(&reg,
        "{\"changes\":{\"provider\":"
        "{\"provider\":\"nokey\",\"base_url\":\"https://n.example/v1\","
        "\"api_key_env\":\"\"}}}");
    assert(ok == NULL);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_error_provider\n");
}


/* 3. reason propagates to $.reason in the parked approval. */
static void test_reason_propagates(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
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
        "{"
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
    sqlite3 *db = test_db_open_seeded(":memory:");
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
    seed_key(db, "OPENROUTER_API_KEY");

    char *result = call_handler(&reg,
        "{\"changes\":{\"provider\":{\"provider\":\"openrouter\"}}}");
    assert(result == NULL);

    sqlite3_stmt *s;
    int rc = sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.changes.provider.base_url'),"
        "       json_extract(args_json,'$.changes.provider.api_key_env')"
        " FROM approvals WHERE session_id=?1 AND park_reason='approval_required'"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL);
    assert(rc == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                 "https://openrouter.ai/api/v1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1),
                 "OPENROUTER_API_KEY") == 0);
    sqlite3_finalize(s);

    /* A7 regression: a deliberately keyless provider survives a provider
     * document. The old canonicalizer re-derived <PROVIDER>_API_KEY whenever
     * api_key_env was absent, and the phantom name made routing drop every
     * one of that provider's models without a word. */
    assert(sqlite3_exec(db,
        "INSERT INTO providers(name, base_url, api_key_env)"
        " VALUES('gateway','http://127.0.0.1:8080/v1','')",
        NULL, NULL, NULL) == SQLITE_OK);
    ctx.current_tool_call_id = "pd2";
    result = call_handler(&reg,
        "{\"changes\":{\"provider\":"
        "{\"provider\":\"gateway\",\"base_url\":\"http://127.0.0.1:9090/v1\"}}}");
    assert(result == NULL);
    assert(sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.changes.provider.api_key_env')"
        " FROM approvals WHERE session_id=?1 ORDER BY id DESC LIMIT 1",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "") == 0);
    sqlite3_finalize(s);

    /* …and it stays empty through the apply, too. */
    assert(sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *args = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    char *receipt = NULL;
    assert(request_config_changes_apply(db, "test", args, 0, 0, &receipt) == 0);
    free(args);
    /* The receipt is a re-read: it names the endpoint now stored. */
    assert(receipt && strstr(receipt, "provider gateway -> "
                                      "http://127.0.0.1:9090/v1 (key: none)"));
    free(receipt);
    assert(sqlite3_prepare_v2(db,
        "SELECT api_key_env FROM providers WHERE name='gateway'",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "") == 0);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_provider_defaults\n");
}

/* The models section: register on an existing provider, update in place,
 * disable — and refuse a model whose provider does not exist. */
static void test_models_section(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);
    seed_key(db, "OPENROUTER_API_KEY");

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "ms1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* The inverted dependency: a model needs its provider to exist first. */
    char *r = call_handler(&reg,
        "{\"changes\":{\"models\":"
        "[{\"id\":\"m1@ghostprov\"}]}}");
    assert(r && strstr(r, "provider 'ghostprov' is not registered"));
    free(r);

    /* A bare id is refused with the canonical form spelled out. */
    ctx.current_tool_call_id = "ms2";
    r = call_handler(&reg,
        "{\"changes\":{\"models\":"
        "[{\"id\":\"just-a-name\"}]}}");
    assert(r && strstr(r, "model@provider"));
    free(r);

    /* Register with metadata onto the seeded provider, then apply. */
    ctx.current_tool_call_id = "ms3";
    r = call_handler(&reg,
        "{\"changes\":{\"models\":"
        "[{\"id\":\"newmodel@openrouter\",\"context_window\":200000,"
        "\"max_output_tokens\":8192,\"capabilities\":[\"text\",\"image\"]}]}}");
    assert(r == NULL);
    char *receipt = NULL;
    apply_latest(db, sid, &receipt);
    assert(receipt && strstr(receipt, "model newmodel@openrouter (healthy, "
                                      "context 200000)"));
    free(receipt);

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT provider_name, model, context_window, max_output_tokens,"
        "       capabilities, status FROM models"
        " WHERE id='newmodel@openrouter'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "openrouter") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "newmodel") == 0);
    assert(sqlite3_column_int(s, 2) == 200000);
    assert(sqlite3_column_int(s, 3) == 8192);
    assert(strstr((const char *)sqlite3_column_text(s, 4), "image") != NULL);
    assert(strcmp((const char *)sqlite3_column_text(s, 5), "healthy") == 0);
    sqlite3_finalize(s);

    /* Update: only the fields present move; the rest keep their values. */
    ctx.current_tool_call_id = "ms4";
    r = call_handler(&reg,
        "{\"changes\":{\"models\":"
        "[{\"id\":\"newmodel@openrouter\",\"context_window\":64000}]}}");
    assert(r == NULL);
    apply_latest(db, sid, NULL);
    assert(sqlite3_prepare_v2(db,
        "SELECT context_window, max_output_tokens, status"
        " FROM models WHERE id='newmodel@openrouter'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 64000);
    assert(sqlite3_column_int(s, 1) == 8192);      /* untouched */
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "healthy") == 0);
    sqlite3_finalize(s);

    /* Disable — a first-class verb now, not a DELETE nobody could request. */
    ctx.current_tool_call_id = "ms5";
    r = call_handler(&reg,
        "{\"changes\":{\"models\":"
        "[{\"id\":\"newmodel@openrouter\",\"status\":\"disabled\"}]}}");
    assert(r == NULL);
    apply_latest(db, sid, NULL);
    assert(sqlite3_prepare_v2(db,
        "SELECT status, context_window FROM models"
        " WHERE id='newmodel@openrouter'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "disabled") == 0);
    assert(sqlite3_column_int(s, 1) == 64000);
    sqlite3_finalize(s);

    /* Typo-hostile like every other section. */
    ctx.current_tool_call_id = "ms6";
    r = call_handler(&reg,
        "{\"changes\":{\"models\":"
        "[{\"id\":\"newmodel@openrouter\",\"ctx\":1}]}}");
    assert(r && strstr(r, "unknown models key 'ctx'"));
    free(r);
    ctx.current_tool_call_id = "ms7";
    r = call_handler(&reg,
        "{\"changes\":{\"models\":"
        "[{\"id\":\"newmodel@openrouter\",\"status\":\"retired\"}]}}");
    assert(r && strstr(r, "'healthy' or 'disabled'"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_models_section\n");
}

/* A bare model name in the agent section gets a did-you-mean, not a shrug —
 * the canonical-id rule is only teachable if the error names the id. */
static void test_bare_model_name_teaching(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "bn1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* 'deepseek/deepseek-v4-flash' is seeded, under a canonical id. */
    char *r = call_handler(&reg,
        "{\"changes\":{\"agent\":"
        "{\"models\":[\"deepseek/deepseek-v4-flash\"]}}}");
    assert(r && strstr(r, "bare model name"));
    assert(r && strstr(r, "openrouter/deepseek/deepseek-v4-flash"));
    free(r);

    /* Nothing like it registered → no invented suggestion. */
    ctx.current_tool_call_id = "bn2";
    r = call_handler(&reg,
        "{\"changes\":{\"agent\":"
        "{\"models\":[\"ghost\"]}}}");
    assert(r && strstr(r, "unknown model 'ghost'"));
    assert(r && !strstr(r, "Did you mean"));
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_bare_model_name_teaching\n");
}


/* 5. Dedup: same doc twice while pending => "already sent"; after marking
 *    first denied, the same doc parks again; a different doc parks while
 *    first is pending. */
static void test_dedup(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
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

    const char *doc = "{"
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
        "{"
        "\"changes\":{\"grants\":{\"hosts\":[\"example.com\"]}}}");
    assert(r == NULL);

    /* Mark the first approval as expired (auto:expired = nobody decided). */
    assert(sqlite3_exec(db,
        "UPDATE approvals SET state='denied', decided_via='auto:expired'"
        " WHERE park_reason='approval_required'"
        " AND json_extract(args_json,'$.changes.grants.tools[0]')='shell_exec'",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Same doc after expiry — must park (expiry is not a denial). */
    ctx.current_tool_call_id = "dd4";
    r = call_handler(&reg, doc);
    assert(r == NULL);

    /* Now a human denies it. */
    assert(sqlite3_exec(db,
        "UPDATE approvals SET state='denied', decided_via='channel:discord'"
        " WHERE state='pending'"
        " AND json_extract(args_json,'$.changes.grants.tools[0]')='shell_exec'",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Same doc after a human denial — refused inline, even with a fresh
     * reason (the guard matches the changes doc, not the commentary). */
    ctx.current_tool_call_id = "dd5";
    r = call_handler(&reg,
        "{"
        "\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}},"
        "\"reason\":\"different wording, same ask\"}");
    assert(r != NULL && strstr(r, "already denied") != NULL);
    free(r);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_dedup\n");
}


/* 6. Batch: one doc with grants+config+provider parks exactly ONE approval;
 *    then call request_config_changes_apply and verify grants, config, and
 *    provider rows landed. */
static void test_batch_apply(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
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

    seed_key(db, "OPENROUTER_API_KEY");
    char *r = call_handler(&reg,
        "{\"changes\":{"
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

    char *receipt = NULL;
    rc = request_config_changes_apply(db, "test", args_copy, 0, 0, &receipt);
    assert(rc == 0);
    /* The receipt is a re-read of every section: the grants that are live,
     * the config value as stored, the provider row as it now stands. */
    assert(receipt != NULL);
    assert(strstr(receipt, "grants now: ") != NULL);
    assert(strstr(receipt, "tool shell_exec") != NULL);
    assert(strstr(receipt, "config now: myext.knob=42") != NULL);
    assert(strstr(receipt, "provider openrouter -> "
                           "https://openrouter.ai/api/v1 "
                           "(key: OPENROUTER_API_KEY)") != NULL);
    free(receipt);
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
        "SELECT base_url, api_key_env"
        " FROM providers WHERE name='openrouter'", -1, &s, NULL);
    assert(rc == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                 "https://openrouter.ai/api/v1") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1),
                 "OPENROUTER_API_KEY") == 0);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_batch_apply\n");
}


/* 6b. agent + routes sections: eager validation and batch apply. */
static void test_agent_routes_sections(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    db_agent_upsert(db, "Other", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    /* A live channel plus a route already owned by another agent. */
    assert(sqlite3_exec(db,
        "INSERT INTO extensions(name,path) VALUES('tgext','/x');"
        "INSERT INTO channels(name,extension_name) VALUES('tg','tgext');"
        "INSERT INTO sessions(name,agent_name,channel_name,chat_id)"
        " VALUES('o','Other','tg','-500');"
        "INSERT INTO channel_routes(channel_name,chat_id,session_id)"
        " VALUES('tg','-500',last_insert_rowid());", NULL, NULL, NULL) == SQLITE_OK);

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
        "{\"changes\":"
        "{\"agent\":{\"sandbox_profile\":\"host\"}}}");
    assert(r && strstr(r, "unknown agent key"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":"
        "{\"agent\":{\"max_iterations\":0}}}");
    assert(r && strstr(r, "positive integer"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":"
        "{\"agent\":{\"models\":[\"ghost-model\"]}}}");
    assert(r && strstr(r, "unknown model"));
    free(r);

    /* routes section: shape, unknown channel, wildcard, foreign owner. */
    r = call_handler(&reg,
        "{\"changes\":{\"routes\":[\"tg\"]}}");
    assert(r && strstr(r, "'channel:chat_id'"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":{\"routes\":[\"nochan:1\"]}}");
    assert(r && strstr(r, "unknown channel"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":{\"routes\":[\"tg:*\"]}}");
    assert(r && strstr(r, "no wildcard routes"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":{\"routes\":[\"tg:-500\"]}}");
    assert(r && strstr(r, "already owned"));
    free(r);

    /* One document, the whole chain: define the transport, register a model
     * on it, adopt that model, take a route. */
    seed_key(db, "GEMINI_API_KEY");
    r = call_handler(&reg,
        "{\"changes\":{"
        "\"provider\":{\"provider\":\"gemini\"},"
        "\"models\":[{\"id\":\"gemini-3.5-flash-lite@gemini\","
        "\"context_window\":1000000}],"
        "\"agent\":{\"models\":[\"gemini-3.5-flash-lite@gemini\"],"
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

    assert(request_config_changes_apply(db, "test", args_copy, 0, 0, NULL) == 0);
    free(args_copy);

    /* models row seeded → the adopted id resolves. */
    assert(sqlite3_prepare_v2(db,
        "SELECT provider_name, model FROM models"
        " WHERE id='gemini-3.5-flash-lite@gemini'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "gemini") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "gemini-3.5-flash-lite") == 0);
    sqlite3_finalize(s);

    /* routing list replaced; untouched scalar columns kept. */
    assert(sqlite3_prepare_v2(db,
        "SELECT (SELECT model_id FROM agent_models"
        "         WHERE agent_name='test' ORDER BY pos LIMIT 1),"
        "       max_iterations, shell_timeout FROM agents"
        " WHERE name='test'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "gemini-3.5-flash-lite@gemini") == 0);
    assert(sqlite3_column_int(s, 1) == 40);
    assert(sqlite3_column_int(s, 2) == 30); /* default untouched */
    sqlite3_finalize(s);

    /* route landed: pinned to an eagerly created session owned by this
     * agent, explicit delivery (send authority only). */
    assert(sqlite3_prepare_v2(db,
        "SELECT se.agent_name, r.delivery_mode, se.channel_name, se.chat_id"
        " FROM channel_routes r JOIN sessions se ON se.id=r.session_id"
        " WHERE r.channel_name='tg' AND r.chat_id='777'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "test") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "explicit") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 2), "tg") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 3), "777") == 0);
    sqlite3_finalize(s);

    /* Route captured by another agent between park and apply → the whole
     * document rolls back (savepoint), including the agent section. */
    r = call_handler(&reg,
        "{\"changes\":{"
        "\"agent\":{\"max_iterations\":99},\"routes\":[\"tg:888\"]}}");
    assert(r == NULL);
    assert(sqlite3_exec(db,
        "INSERT INTO sessions(name,agent_name,channel_name,chat_id)"
        " VALUES('o2','Other','tg','888');"
        "INSERT INTO channel_routes(channel_name,chat_id,session_id)"
        " VALUES('tg','888',last_insert_rowid());", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    args_copy = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(request_config_changes_apply(db, "test", args_copy, 0, 0, NULL) == -1);
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


/* agent.models entries as {id, effort} objects: validation of the union
 * shape, effort landing in agent_models.reasoning_effort, and the whole-list
 * replace clearing effort when a later doc reverts to the bare-string form. */
static void test_agent_models_effort(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "ef1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);
    assert(session_set_state(db, sid, "tool_running") == 0);

    /* Shape errors: bad effort level, unknown object key, non-entry type,
     * object without an id, duplicate id across the two spellings. */
    char *r = call_handler(&reg,
        "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"openrouter/deepseek/deepseek-v4-flash\",\"effort\":\"max\"}]}}}");
    assert(r && strstr(r, "effort 'max' must be one of"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"openrouter/deepseek/deepseek-v4-flash\",\"reasoning\":\"high\"}]}}}");
    assert(r && strstr(r, "unknown agent.models key 'reasoning'"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":{\"agent\":{\"models\":[7]}}}");
    assert(r && strstr(r, "model id string or an {id, effort} object"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":{\"agent\":{\"models\":[{\"effort\":\"high\"}]}}}");
    assert(r && strstr(r, "non-empty canonical model id"));
    free(r);
    r = call_handler(&reg,
        "{\"changes\":{\"agent\":{\"models\":"
        "[\"openrouter/deepseek/deepseek-v4-flash\","
        "{\"id\":\"openrouter/deepseek/deepseek-v4-flash\",\"effort\":\"low\"}]}}}");
    assert(r && strstr(r, "duplicate entry"));
    free(r);

    /* Object form parks and applies: effort lands on the routing row. */
    r = call_handler(&reg,
        "{\"changes\":{\"agent\":{\"models\":"
        "[{\"id\":\"openrouter/deepseek/deepseek-v4-flash\",\"effort\":\"high\"}]}}}");
    assert(r == NULL); /* parked */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *args_copy = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(request_config_changes_apply(db, "test", args_copy, 0, 0, NULL) == 0);
    free(args_copy);
    assert(sqlite3_prepare_v2(db,
        "SELECT model_id, reasoning_effort FROM agent_models"
        " WHERE agent_name='test' ORDER BY pos", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "openrouter/deepseek/deepseek-v4-flash") == 0);
    assert(strcmp((const char *)sqlite3_column_text(s, 1), "high") == 0);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);

    /* Bare-string replacement clears the effort (whole-list replace). */
    ctx.current_tool_call_id = "ef2";
    r = call_handler(&reg,
        "{\"changes\":{\"agent\":{\"models\":"
        "[\"openrouter/deepseek/deepseek-v4-flash\"]}}}");
    assert(r == NULL);
    assert(sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1"
        " ORDER BY id DESC LIMIT 1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    args_copy = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    assert(request_config_changes_apply(db, "test", args_copy, 0, 0, NULL) == 0);
    free(args_copy);
    assert(sqlite3_prepare_v2(db,
        "SELECT reasoning_effort IS NULL FROM agent_models"
        " WHERE agent_name='test'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_agent_models_effort\n");
}


/* 7. Rollback: hand-craft args_json with an unregistered config key, call
 *    request_config_changes_apply, assert -1 AND none of the doc's grants
 *    landed. */
static void test_apply_rollback(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);

    /* Hand-crafted args_json that has a valid tool grant AND an invalid
     * config key (bypass handler validation by constructing the literal). */
    const char *bad_args =
        "{\"changes\":{"
        "\"grants\":{\"tools\":[\"web_fetch\"]},"
        "\"config\":{\"nonexistent_key_xyz\":\"boom\"}}}";

    int rc = request_config_changes_apply(db, "test", bad_args, 0, 0, NULL);
    assert(rc == -1); /* config_set fails for unregistered key */

    /* Verify the tool grant did NOT land (rollback). */
    AgentCaps caps;
    agent_caps_load(db, "test", &caps);
    assert(caps.tool_count == 0);
    agent_caps_free(&caps);

    db_close(db);
    printf("  PASS test_apply_rollback\n");
}



/* Direct agent_config_grant still works (low-level API). */
static void test_add_tool_to_config(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
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


/* A path grant covering cclaw.db is refused at request time, with the reason —
 * it never parks, so no approver is ever shown a grant that would be inert.
 * Needs an on-disk DB: the rule resolves the handle's own filename.
 *
 * Positive control in the same function: an ordinary path grant still parks. */
static void test_db_path_grant_refused(void) {
    const char *dbp = "/tmp/cclaw_reqcfg_grantdb/cclaw.db";
    system("rm -rf /tmp/cclaw_reqcfg_grantdb && mkdir -p /tmp/cclaw_reqcfg_grantdb");
    sqlite3 *db = test_db_open_seeded(dbp);
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "d1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* Positive control: an unrelated path parks (NULL = parked, not an error). */
    char *r = call_handler(&reg,
        "{\"changes\":"
        "{\"grants\":{\"read_paths\":[\"/usr/share\"]}}}");
    assert(r == NULL || strstr(r, "error:") == NULL);
    free(r);

    ctx.current_tool_call_id = "d2";
    r = call_handler(&reg,
        "{\"changes\":"
        "{\"grants\":{\"read_paths\":[\"/tmp/cclaw_reqcfg_grantdb/cclaw.db\"]}}}");
    assert(r != NULL && strstr(r, "cclaw.db") != NULL);
    free(r);

    /* The containing directory is the realistic ask, and is refused too. */
    ctx.current_tool_call_id = "d3";
    r = call_handler(&reg,
        "{\"changes\":"
        "{\"grants\":{\"write_paths\":[\"/tmp/cclaw_reqcfg_grantdb\"]}}}");
    assert(r != NULL && strstr(r, "cclaw.db") != NULL);
    free(r);

    tools_free(&reg);
    db_close(db);
    system("rm -rf /tmp/cclaw_reqcfg_grantdb");
    printf("  PASS test_db_path_grant_refused\n");
}

/* Redundant requests: fully-satisfied documents return a helpful result
 * instead of parking; partially-satisfied ones park only the remainder. */
static void test_redundant_filtered(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);
    assert(sid > 0);

    assert(agent_config_grant(db, "test", "host", "api.tiingo.com", 0) == 0);
    assert(agent_config_grant(db, "test", "tool", "js_eval", 0) == 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "r1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);
    assert(session_set_state(db, sid, "tool_running") == 0);

    /* Fully redundant: no approval parked, session state untouched. */
    char *result = call_handler(&reg,
        "{\"changes\":"
        "{\"grants\":{\"hosts\":[\"api.tiingo.com\"],\"tools\":[\"js_eval\"]}}}");
    assert(result != NULL && strstr(result, "already in effect") != NULL);
    free(result);
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM approvals WHERE session_id=?1", -1, &s, NULL)
        == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 0);
    sqlite3_finalize(s);
    assert(sqlite3_prepare_v2(db,
        "SELECT state FROM sessions WHERE id=?1", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "tool_running") == 0);
    sqlite3_finalize(s);

    /* Partially redundant: parked document keeps only the new host. */
    ctx.current_tool_call_id = "r2";
    result = call_handler(&reg,
        "{\"changes\":"
        "{\"grants\":{\"hosts\":[\"api.tiingo.com\",\"example.com\"]}}}");
    assert(result == NULL); /* parked */
    assert(sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.changes.grants.hosts')"
        " FROM approvals WHERE session_id=?1 AND state='pending'",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "[\"example.com\"]") == 0);
    sqlite3_finalize(s);

    /* Redundant grants alongside a live section: grants stripped, agent
     * section still parks. */
    ctx.current_tool_call_id = "r3";
    result = call_handler(&reg,
        "{\"changes\":"
        "{\"grants\":{\"tools\":[\"js_eval\"]},\"agent\":{\"max_iterations\":9}}}");
    assert(result == NULL); /* parked */
    assert(sqlite3_prepare_v2(db,
        "SELECT json_type(args_json,'$.changes.grants'),"
        "       json_extract(args_json,'$.changes.agent.max_iterations')"
        " FROM approvals WHERE session_id=?1 AND state='pending'"
        " AND tool_call_id='r3'", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_type(s, 0) == SQLITE_NULL); /* grants removed */
    assert(sqlite3_column_int(s, 1) == 9);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_redundant_filtered\n");
}

/* secret_bindings section: validation rejections, park, apply mints
 * secret_hosts rows, redundancy filter. */
static void test_secret_bindings_section(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);
    assert(sid > 0);
    assert(sqlite3_exec(db,
        "INSERT INTO secrets(name,value,scope) VALUES"
        " ('ALPACA_KEY','enc:00','agent'),('SYS_KEY','enc:00','system')",
        NULL, NULL, NULL) == SQLITE_OK);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "sb1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* Unknown secret name is refused — a binding request is not where a
     * secret is born. */
    char *r = call_handler(&reg,
        "{\"changes\":{"
        "\"secret_bindings\":{\"NOPE\":[\"api.example.com\"]}}}");
    assert(r && strstr(r, "unknown secret 'NOPE'"));
    free(r);

    /* System-scoped secrets never interpolate — binding one is refused. */
    r = call_handler(&reg,
        "{\"changes\":{"
        "\"secret_bindings\":{\"SYS_KEY\":[\"api.example.com\"]}}}");
    assert(r && strstr(r, "unknown secret 'SYS_KEY'"));
    free(r);

    /* Host shape: no wildcards, no bare TLD, no single label. */
    static const char *bad_hosts[] = {"*.example.com", ".com", "localhost"};
    for (size_t i = 0; i < 3; i++) {
        char args[192];
        snprintf(args, sizeof(args),
            "{\"changes\":{"
            "\"secret_bindings\":{\"ALPACA_KEY\":[\"%s\"]}}}", bad_hosts[i]);
        r = call_handler(&reg, args);
        assert(r && strstr(r, "not a valid hostname"));
        free(r);
    }

    /* Value must be an array. */
    r = call_handler(&reg,
        "{\"changes\":{"
        "\"secret_bindings\":{\"ALPACA_KEY\":\"api.example.com\"}}}");
    assert(r && strstr(r, "must be an array"));
    free(r);

    /* Valid document parks exactly one approval carrying the section. */
    r = call_handler(&reg,
        "{\"changes\":{"
        "\"secret_bindings\":{\"ALPACA_KEY\":"
        "[\"api.alpaca.markets\",\".example.com\"]}}}");
    assert(r == NULL); /* parked */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT args_json FROM approvals WHERE session_id=?1"
        " AND state='pending' AND tool_call_id='sb1'", -1, &s, NULL)
        == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    char *args_copy = strdup((const char *)sqlite3_column_text(s, 0));
    assert(args_copy && strstr(args_copy, "secret_bindings"));
    sqlite3_finalize(s);

    /* No rows minted while the approval is merely pending. */
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM secret_hosts", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 0);
    sqlite3_finalize(s);

    /* Apply mints both pairs, durably. */
    assert(request_config_changes_apply(db, "test", args_copy, 0, 0, NULL) == 0);
    free(args_copy);
    assert(sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM secret_hosts WHERE secret_name='ALPACA_KEY'"
        " AND host IN ('api.alpaca.markets','.example.com')", -1, &s, NULL)
        == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW && sqlite3_column_int(s, 0) == 2);
    sqlite3_finalize(s);

    /* Redundancy: the recorded pairs no longer park. */
    ctx.current_tool_call_id = "sb2";
    assert(session_set_state(db, sid, "tool_running") == 0);
    r = call_handler(&reg,
        "{\"changes\":{"
        "\"secret_bindings\":{\"ALPACA_KEY\":[\"api.alpaca.markets\"]}}}");
    assert(r && strstr(r, "already in effect"));
    free(r);

    /* Partially redundant: parked document keeps only the new host. */
    ctx.current_tool_call_id = "sb3";
    r = call_handler(&reg,
        "{\"changes\":{"
        "\"secret_bindings\":{\"ALPACA_KEY\":"
        "[\"api.alpaca.markets\",\"api.tiingo.com\"]}}}");
    assert(r == NULL); /* parked */
    assert(sqlite3_prepare_v2(db,
        "SELECT json_extract(args_json,'$.changes.secret_bindings.ALPACA_KEY')"
        " FROM approvals WHERE session_id=?1 AND state='pending'"
        " AND tool_call_id='sb3'", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0),
                  "[\"api.tiingo.com\"]") == 0);
    sqlite3_finalize(s);

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_secret_bindings_section\n");
}

/* grants.remove — narrowing applies immediately and never parks. */
static void test_grants_remove(void) {
    sqlite3 *db = test_db_open_seeded(":memory:");
    assert(db);
    config_registry_sync(db);
    db_agent_upsert(db, "test", NULL, NULL);
    int64_t sid = session_create(db, "t", "test", -1, 0);
    assert(agent_config_grant(db, "test", "host", "api.example.com", 0) == 0);
    assert(agent_config_grant(db, "test", "tool", "shell_exec", 0) == 0);

    RequestConfigCtx ctx = {
        .db = db, .agent_name = "test", .session_id = sid,
        .agents_dir = NULL, .current_tool_call_id = "rm1"
    };
    ToolRegistry reg;
    tools_init(&reg);
    tool_request_config_register(&reg, &ctx);

    /* Happy path: applied here and now, no approval row, receipt names it. */
    char *r = call_handler(&reg,
        "{\"changes\":{\"grants\":{\"remove\":{\"hosts\":[\"api.example.com\"]}}}}");
    assert(r != NULL && strstr(r, "api.example.com") && strstr(r, "removed"));
    free(r);
    assert(!row_exists(db, "SELECT 1 FROM grants WHERE agent_name='test'"
                           " AND kind='host' AND value='api.example.com'"));
    assert(!row_exists(db, "SELECT 1 FROM approvals WHERE session_id > 0"));
    /* Untouched kinds stay. */
    assert(row_exists(db, "SELECT 1 FROM grants WHERE kind='tool'"
                          " AND value='shell_exec'"));

    /* Unknown kind refuses — and cannot reach containment settings. */
    ctx.current_tool_call_id = "rm2";
    r = call_handler(&reg,
        "{\"changes\":{\"grants\":{\"remove\":{\"sandbox_profile\":[\"host\"]}}}}");
    assert(r != NULL && strstr(r, "sandbox_profile") && strstr(r, "error"));
    free(r);

    /* A value that is not a live grant refuses, naming it. */
    ctx.current_tool_call_id = "rm3";
    r = call_handler(&reg,
        "{\"changes\":{\"grants\":{\"remove\":{\"hosts\":[\"nope.example\"]}}}}");
    assert(r != NULL && strstr(r, "nope.example") && strstr(r, "not one of"));
    free(r);
    assert(row_exists(db, "SELECT 1 FROM grants WHERE kind='tool'"
                          " AND value='shell_exec'"));

    /* Mixed remove + widen: the removal lands now, the rest parks. */
    ctx.current_tool_call_id = "rm4";
    assert(session_set_state(db, sid, "tool_running") == 0);
    r = call_handler(&reg,
        "{\"changes\":{\"grants\":{\"hosts\":[\"new.example.com\"],"
        "\"remove\":{\"tools\":[\"shell_exec\"]}}}}");
    assert(r == NULL);                     /* parked */
    assert(!row_exists(db, "SELECT 1 FROM grants WHERE kind='tool'"
                           " AND value='shell_exec'"));
    /* The parked document carries the widening only; the removal already
     * happened and rides the approver-facing reason. */
    assert(row_exists(db,
        "SELECT 1 FROM approvals WHERE state='pending'"
        "   AND json_extract(args_json,'$.changes.grants.hosts[0]')"
        "       ='new.example.com'"
        "   AND json_extract(args_json,'$.changes.grants.remove') IS NULL"
        "   AND json_extract(args_json,'$.reason') LIKE '%shell_exec%'"));

    tools_free(&reg);
    db_close(db);
    printf("  PASS test_grants_remove\n");
}

int main(void) {
    TEST_INIT();
    printf("test_request_config:\n");
    test_register();
    test_handler_unavailable();
    test_park_tools_grant();
    test_error_missing_changes();
    test_db_path_grant_refused();
    test_error_config_keys();
    test_error_provider();
    test_reason_propagates();
    test_provider_defaults();
    test_models_section();
    test_bare_model_name_teaching();
    test_dedup();
    test_batch_apply();
    test_agent_routes_sections();
    test_agent_models_effort();
    test_apply_rollback();
    test_add_tool_to_config();
    test_redundant_filtered();
    test_secret_bindings_section();
    test_grants_remove();
    printf("\nAll request_config tests passed.\n");
    return 0;
}
