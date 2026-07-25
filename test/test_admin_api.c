#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "admin_api.h"
#include "db.h"
#include "test_util.h"
#include <sqlite3.h>
#include "agent_config.h"

#define DB_PATH "/tmp/test_admin_api.db"

static const uint8_t TEST_KEY[32] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32
};

static sqlite3 *setup_db(void) {
    test_db_clean(DB_PATH);
    sqlite3 *db = test_db_open_seeded(DB_PATH);
    assert(db);
    db_set_secret_key(TEST_KEY);
    return db;
}

static void test_key_env_name(void) {
    sqlite3 *db = setup_db();
    char *v = admin_key_env_name(db, "openrouter");
    assert(v && strcmp(v, "OPENROUTER_API_KEY") == 0);
    free(v);
    v = admin_key_env_name(db, "gemini");
    assert(v && strcmp(v, "GEMINI_API_KEY") == 0);
    free(v);
    assert(admin_key_env_name(db, "unknown") == NULL);
    assert(admin_key_env_name(db, NULL) == NULL);
    assert(admin_key_env_name(NULL, "openrouter") == NULL);
    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_key_env_name\n");
}

static void test_set_key_known_provider(void) {
    sqlite3 *db = setup_db();
    assert(admin_set_key(db, "openrouter", "sk-test-123") == 0);

    /* Stored encrypted under the canonical env-var name in secrets table */
    char *val = db_secret_get_system(db, "OPENROUTER_API_KEY");
    assert(val && strcmp(val, "sk-test-123") == 0);
    free(val);

    /* Raw value in secrets table has enc: prefix */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT value FROM secrets WHERE name='OPENROUTER_API_KEY'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *raw = (const char *)sqlite3_column_text(s, 0);
    assert(raw && strncmp(raw, "enc:", 4) == 0);
    sqlite3_finalize(s);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_set_key_known_provider\n");
}

/* A provider row with a blank api_key_env — the schema default, and what the
 * dashboard's add-provider form stores — used to make admin_set_key return -1,
 * which the dashboard collapsed into a generic "action failed" 400. The row now
 * adopts <PROVIDER>_API_KEY and the key lands there. */
static void test_set_key_adopts_blank_env_name(void) {
    sqlite3 *db = setup_db();
    assert(sqlite3_exec(db,
        "INSERT INTO providers(name, base_url, endpoint_type, api_key_env,"
        " default_model, priority)"
        " VALUES('my-vllm','https://vllm.example/v1','openai','','local-7b',200);",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Pure lookup still reports "unnamed" — it does not mutate. */
    assert(admin_key_env_name(db, "my-vllm") == NULL);

    assert(admin_set_key(db, "my-vllm", "sk-vllm-xyz") == 0);

    /* The row was named, and named deterministically. */
    char *env = admin_key_env_name(db, "my-vllm");
    assert(env && strcmp(env, "MY_VLLM_API_KEY") == 0);   /* '-' folds to '_' */
    free(env);

    char *val = db_secret_get_system(db, "MY_VLLM_API_KEY");
    assert(val && strcmp(val, "sk-vllm-xyz") == 0);
    free(val);

    /* Idempotent: a second save reuses the adopted name, not a new one. */
    assert(admin_set_key(db, "my-vllm", "sk-vllm-2") == 0);
    val = db_secret_get_system(db, "MY_VLLM_API_KEY");
    assert(val && strcmp(val, "sk-vllm-2") == 0);
    free(val);

    /* A provider that does not exist at all is still an error. */
    assert(admin_set_key(db, "no-such-provider", "sk-nope") == -1);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_set_key_adopts_blank_env_name\n");
}

static void test_set_key_custom(void) {
    sqlite3 *db = setup_db();
    assert(admin_set_key(db, "custom", "MY_VAR=secret") == 0);

    char *val = db_secret_get_system(db, "MY_VAR");
    assert(val && strcmp(val, "secret") == 0);
    free(val);

    /* Malformed custom values rejected */
    assert(admin_set_key(db, "custom", "no-equals") == -1);
    assert(admin_set_key(db, "custom", "=leading-eq") == -1);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_set_key_custom\n");
}

static void test_set_model_primary(void) {
    sqlite3 *db = setup_db();
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,default_model,priority)"
        " VALUES('openrouter','https://openrouter.ai/api/v1','OPENROUTER_API_KEY','old-model',0);",
        NULL, NULL, NULL);

    assert(admin_set_model(db, "openrouter", "new-model") == 0);
    /* Unknown provider is an error, not a silent no-op on whoever is first. */
    assert(admin_set_model(db, "no-such-provider", "x") == -1);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT default_model FROM providers WHERE name='openrouter'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *val = (const char *)sqlite3_column_text(s, 0);
    assert(val && strcmp(val, "new-model") == 0);
    sqlite3_finalize(s);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_set_model_primary\n");
}

static void test_set_endpoint_primary(void) {
    sqlite3 *db = setup_db();
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,priority)"
        " VALUES('openrouter','https://old.api/v1','OPENROUTER_API_KEY',0);",
        NULL, NULL, NULL);

    assert(admin_set_endpoint(db, "openrouter", "https://new.api/v1") == 0);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT base_url FROM providers WHERE name='openrouter'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *val = (const char *)sqlite3_column_text(s, 0);
    assert(val && strcmp(val, "https://new.api/v1") == 0);
    sqlite3_finalize(s);

    /* Reject non-http URLs */
    assert(admin_set_endpoint(db, "openrouter", "ftp://bad.url") == -1);
    assert(admin_set_endpoint(db, "no-such-provider", "https://ok/v1") == -1);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_set_endpoint_primary\n");
}

static void test_set_model_fallback(void) {
    sqlite3 *db = setup_db();
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,default_model,priority)"
        " VALUES('openrouter','https://openrouter.ai/api/v1','OPENROUTER_API_KEY','primary',0);",
        NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT OR REPLACE INTO providers(name,base_url,api_key_env,default_model,priority)"
        " VALUES('fallback','https://fb.api/v1','FB_KEY','fb-old',1);",
        NULL, NULL, NULL);

    assert(admin_set_model(db, "fallback", "fb-new") == 0);
    /* Naming the target means the primary is untouched — an index could not
     * express "this one" once priorities shift. */
    assert(admin_set_model(db, "openrouter", "primary-kept") == 0);

    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT default_model FROM providers WHERE name='fallback'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *val = (const char *)sqlite3_column_text(s, 0);
    assert(val && strcmp(val, "fb-new") == 0);
    sqlite3_finalize(s);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_set_model_fallback\n");
}

static void test_admin_grant_capability(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "testagent", NULL, NULL);

    assert(admin_grant_capability(db, "testagent", "host", "example.com") == 0);
    assert(admin_grant_capability(db, "testagent", "read_path", "/tmp") == 0);

    AgentCaps caps;
    agent_caps_load(db, "testagent", &caps);
    assert(caps.host_count == 1);
    assert(strcmp(caps.hosts[0], "example.com") == 0);
    assert(caps.read_count == 1);
    agent_caps_free(&caps);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_admin_grant_capability\n");
}

static void test_admin_list_grants(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "testagent", NULL, NULL);
    db_agent_upsert(db, "otheragent", NULL, NULL);

    assert(admin_grant_capability(db, "testagent", "host", "example.com") == 0);
    assert(admin_grant_capability(db, "testagent", "tool", "shell_exec") == 0);
    assert(admin_grant_capability(db, "testagent", "read_path", "/tmp") == 0);
    assert(admin_grant_capability(db, "otheragent", "host", "other.example") == 0);

    AdminGrant *list = NULL;
    size_t count = 0;
    assert(admin_list_grants(db, "testagent", &list, &count) == 0);
    assert(count == 3);
    for (size_t i = 0; i < count; i++) {
        assert(list[i].id > 0);
        assert(list[i].kind && list[i].value);
    }
    admin_grants_free(list, count);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_admin_list_grants\n");
}

static void test_admin_revoke_grant_by_id(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "testagent", NULL, NULL);
    assert(admin_grant_capability(db, "testagent", "host", "example.com") == 0);
    assert(admin_grant_capability(db, "testagent", "host", "api.openai.com") == 0);

    AdminGrant *list = NULL;
    size_t count = 0;
    assert(admin_list_grants(db, "testagent", &list, &count) == 0);
    assert(count == 2);
    int64_t victim = list[0].id;
    const char *survivor_value = strdup(list[1].value);
    admin_grants_free(list, count);

    assert(admin_revoke_grant_by_id(db, victim) == 0);

    AgentCaps caps;
    agent_caps_load(db, "testagent", &caps);
    assert(caps.host_count == 1);
    assert(strcmp(caps.hosts[0], survivor_value) == 0);
    agent_caps_free(&caps);
    free((void *)survivor_value);

    /* A stale/unknown id fails without touching other rows */
    assert(admin_revoke_grant_by_id(db, victim) != 0);
    agent_caps_load(db, "testagent", &caps);
    assert(caps.host_count == 1);
    agent_caps_free(&caps);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_admin_revoke_grant_by_id\n");
}

static void test_admin_list_tool_names(void) {
    sqlite3 *db = setup_db();
    /* Drop seeded built-in tools — this test asserts an exact listing. */
    sqlite3_exec(db, "DELETE FROM tools;", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO tools(name) VALUES('zeta_tool');", NULL, NULL, NULL);
    sqlite3_exec(db, "INSERT INTO tools(name) VALUES('alpha_tool');", NULL, NULL, NULL);

    char **names = NULL;
    size_t count = 0;
    assert(admin_list_tool_names(db, &names, &count) == 0);
    assert(count == 2);
    assert(strcmp(names[0], "alpha_tool") == 0);
    assert(strcmp(names[1], "zeta_tool") == 0);
    admin_tool_names_free(names, count);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_admin_list_tool_names\n");
}

static int64_t insert_approval(sqlite3 *db, int64_t sid, const char *tool_name,
                               const char *action, const char *args_json, const char *state) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO approvals(session_id, tool_call_id, tool_name, action, args_json, resolve, state)"
        " VALUES(?1,'call_0',?2,?3,?4,'apply',?5);", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_text(s, 2, tool_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, action, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 4, args_json, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 5, state, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    int64_t id = sqlite3_last_insert_rowid(db);
    sqlite3_finalize(s);
    return id;
}

static void set_session_channel(sqlite3 *db, int64_t sid, const char *channel_name) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "UPDATE sessions SET channel_name=? WHERE id=?;",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 2, sid);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

static void test_list_pending_approvals(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "testagent", NULL, NULL);
    int64_t sid = session_create(db, "s1", "testagent", -1, 0);
    set_session_channel(db, sid, "telegram");
    int64_t sid_other = session_create(db, "s2", "testagent", -1, 0);
    set_session_channel(db, sid_other, "other-channel");

    int64_t aid = insert_approval(db, sid, "shell_exec", "run", "{}", "pending");
    insert_approval(db, sid, "request_config", "request_changes",
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"denied.example\"]}}}", "denied");
    insert_approval(db, sid_other, "shell_exec", "run", "{}", "pending");

    AdminApproval *list = NULL;
    size_t count = 0;
    assert(admin_list_pending_approvals(db, "telegram", &list, &count) == 0);
    assert(count == 1);
    assert(list[0].id == aid);
    assert(strcmp(list[0].agent_name, "testagent") == 0);
    assert(strcmp(list[0].tool_name, "shell_exec") == 0);
    admin_approvals_free(list, count);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_list_pending_approvals\n");
}

static void test_list_denied_approvals_grantable_only(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "testagent", NULL, NULL);
    int64_t sid = session_create(db, "s1", "testagent", -1, 0);
    set_session_channel(db, sid, "telegram");

    /* Only the request_config denial is "grantable"; a denied plain tool
     * call (shell_exec) has no standing capability to re-apply. */
    int64_t grantable = insert_approval(db, sid, "request_config", "request_changes",
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"denied.example\"]}}}", "denied");
    insert_approval(db, sid, "shell_exec", "run", "{}", "denied");

    AdminApproval *list = NULL;
    size_t count = 0;
    assert(admin_list_denied_approvals(db, "telegram", 10, &list, &count) == 0);
    assert(count == 1);
    assert(list[0].id == grantable);
    admin_approvals_free(list, count);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_list_denied_approvals_grantable_only\n");
}

static void test_grant_from_history(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "testagent", NULL, NULL);
    int64_t sid = session_create(db, "s1", "testagent", -1, 0);
    set_session_channel(db, sid, "telegram");

    int64_t aid = insert_approval(db, sid, "request_config", "request_changes",
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"reconsidered.example\"]}}}", "denied");

    assert(admin_grant_from_history(db, aid) == 0);

    /* The grant actually landed */
    AgentCaps caps;
    agent_caps_load(db, "testagent", &caps);
    assert(caps.host_count == 1);
    assert(strcmp(caps.hosts[0], "reconsidered.example") == 0);
    agent_caps_free(&caps);

    /* Original denied row is untouched; a new approved row was recorded */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT state FROM approvals WHERE id=?;", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, aid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "denied") == 0);
    sqlite3_finalize(s);

    assert(sqlite3_prepare_v2(db,
        "SELECT count(*) FROM approvals WHERE state='approved' AND decided_via='channel:telegram:history_grant';",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);

    /* A non-grantable (plain tool-call) denial is rejected */
    int64_t bad = insert_approval(db, sid, "shell_exec", "run", "{}", "denied");
    assert(admin_grant_from_history(db, bad) == -1);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_grant_from_history\n");
}

static void test_list_models(void) {
    sqlite3 *db = setup_db();
    unsetenv("TEST_MODELS_KEY");
    /* Drop seeded provider/model rows — this test asserts an exact listing. */
    sqlite3_exec(db, "DELETE FROM models; DELETE FROM providers;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "INSERT INTO providers(name,base_url,api_key_env,priority)"
        " VALUES('p1','https://p1.example/v1','TEST_MODELS_KEY',0);"
        "INSERT INTO models(id,provider_name,model,context_window,priority,status,"
        "                   error_count_5xx,degraded_until) VALUES"
        " ('p1/alpha','p1','alpha',NULL,0,'healthy',0,NULL),"
        " ('p1/beta','p1','beta',200000,1,'degraded',3,unixepoch()+120);",
        NULL, NULL, NULL);

    AdminModel *list = NULL;
    size_t count = 0;
    assert(admin_list_models(db, &list, &count) == 0);
    assert(count == 2);
    assert(strcmp(list[0].id, "p1/alpha") == 0);
    assert(list[0].context_window == 0);          /* NULL → 0 = default */
    assert(list[0].has_key == 0);                  /* no env, no secret */
    assert(list[0].degraded_left == 0);
    assert(strcmp(list[1].id, "p1/beta") == 0);
    assert(list[1].context_window == 200000);
    assert(list[1].degraded_left > 0 && list[1].degraded_left <= 120);
    assert(list[1].err_5xx == 3);
    admin_models_free(list, count);

    /* Key presence via encrypted kv */
    assert(db_secret_set(db, "TEST_MODELS_KEY", "sk-test", "operator", "system") == 0);
    assert(admin_list_models(db, &list, &count) == 0);
    assert(count == 2 && list[0].has_key == 1);
    admin_models_free(list, count);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_list_models\n");
}

static void test_switch_model(void) {
    sqlite3 *db = setup_db();
    sqlite3_exec(db, "DELETE FROM models; DELETE FROM providers;", NULL, NULL, NULL);
    sqlite3_exec(db,
        "INSERT INTO providers(name,base_url,api_key_env,priority)"
        " VALUES('p1','https://p1.example/v1','K1',0);"
        "INSERT INTO models(id,provider_name,model,priority,status,degraded_until,error_count_5xx)"
        " VALUES ('p1/alpha','p1','alpha',0,'healthy',NULL,0),"
        "        ('p1/beta','p1','beta',1,'degraded',unixepoch()+300,5);"
        "INSERT INTO agents(name,primary_model) VALUES('tracker','p1/alpha'),('pinned','other/x');",
        NULL, NULL, NULL);

    char prev[128];
    assert(admin_switch_model(db, "p1/beta", prev, sizeof(prev)) == 0);
    assert(strcmp(prev, "p1/alpha") == 0);

    /* beta now heads the routing order with fresh health */
    AdminModel *list = NULL;
    size_t count = 0;
    assert(admin_list_models(db, &list, &count) == 0);
    assert(count == 2);
    assert(strcmp(list[0].id, "p1/beta") == 0);
    assert(strcmp(list[0].status, "healthy") == 0);
    assert(list[0].degraded_left == 0 && list[0].err_5xx == 0);
    assert(strcmp(list[1].id, "p1/alpha") == 0);   /* previous head = first fallback */
    admin_models_free(list, count);

    /* Agents tracking the old head follow; explicit other pins don't */
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db, "SELECT primary_model FROM agents WHERE name='tracker'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "p1/beta") == 0);
    sqlite3_finalize(s);
    assert(sqlite3_prepare_v2(db, "SELECT primary_model FROM agents WHERE name='pinned'", -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "other/x") == 0);
    sqlite3_finalize(s);

    /* Unknown model refused */
    assert(admin_switch_model(db, "no/such", prev, sizeof(prev)) == -1);

    db_close(db);
    test_db_clean(DB_PATH);
    printf("  PASS: test_switch_model\n");
}

int main(void) {
    TEST_INIT();
    printf("test_admin_api:\n");
    test_key_env_name();
    test_set_key_known_provider();
    test_set_key_adopts_blank_env_name();
    test_set_key_custom();
    test_set_model_primary();
    test_set_endpoint_primary();
    test_set_model_fallback();
    test_admin_grant_capability();
    test_admin_list_grants();
    test_admin_revoke_grant_by_id();
    test_admin_list_tool_names();
    test_list_models();
    test_switch_model();
    test_list_pending_approvals();
    test_list_denied_approvals_grantable_only();
    test_grant_from_history();
    printf("all admin_api tests passed\n");
    return 0;
}
