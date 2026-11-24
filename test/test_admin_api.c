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
}

static void test_grant_from_history_provenance(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "histagent", NULL, NULL);
    int64_t sid = session_create(db, "h", "histagent", -1, 0);
    assert(sid > 0);
    assert(sqlite3_exec(db,
        "INSERT INTO approvals(id, session_id, tool_name, park_reason, args_json,"
        " resolve, state, decided_via) VALUES(50, 1, 'request_config',"
        " 'approval_required', '{\"changes\":{\"grants\":{\"hosts\":[\"hist.example\"]}}}',"
        " 'apply', 'denied', 'channel:x')", NULL, NULL, NULL) == SQLITE_OK);
    assert(sqlite3_exec(db, "UPDATE approvals SET session_id=(SELECT MAX(id) FROM sessions)"
                        " WHERE id=50", NULL, NULL, NULL) == SQLITE_OK);
    db_set_actor(db, "dashboard");
    assert(admin_grant_from_history(db, 50, "dashboard") == 0);
    int64_t nid = db_scalar_i64(db, "SELECT MAX(id) FROM approvals", 0, -1);
    assert(nid > 50);
    assert(db_scalar_i64(db, "SELECT state='denied' FROM approvals WHERE id=50", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT state='approved' FROM approvals WHERE id=?1", nid, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE agent_name='histagent'"
        " AND value='hist.example' AND source='approval' AND approval_id=?1", nid, -1) == 1);
    char want[64];
    snprintf(want, sizeof(want), "approval:%lld", (long long)nid);
    char *ctx = db_scalar_text(db, "SELECT context FROM change_log WHERE table_name='grants'"
        " AND actor='dashboard' ORDER BY id DESC LIMIT 1", 0);
    assert(ctx && strcmp(ctx, want) == 0);
    free(ctx);
    /* The handle's label does not outlive the work. */
    char now[16];
    db_get_context(db, now, sizeof(now));
    assert(now[0] == '\0');
    db_close(db);
    test_db_clean(DB_PATH);
}

static int64_t insert_approval(sqlite3 *db, int64_t sid, const char *tool_name,
                               const char *park_reason, const char *args_json, const char *state) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO approvals(session_id, tool_call_id, tool_name, park_reason, args_json, resolve, state)"
        " VALUES(?1,'call_0',?2,?3,?4,'apply',?5);", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_text(s, 2, tool_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, park_reason, -1, SQLITE_STATIC);
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
}

static void test_grant_from_history(void) {
    sqlite3 *db = setup_db();
    db_agent_upsert(db, "testagent", NULL, NULL);
    int64_t sid = session_create(db, "s1", "testagent", -1, 0);
    set_session_channel(db, sid, "telegram");

    int64_t aid = insert_approval(db, sid, "request_config", "request_changes",
        "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"reconsidered.example\"]}}}", "denied");

    assert(admin_grant_from_history(db, aid, "cli") == 0);

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
        "SELECT count(*) FROM approvals WHERE state='approved' AND decided_via='cli';",
        -1, &s, NULL) == SQLITE_OK);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(sqlite3_column_int(s, 0) == 1);
    sqlite3_finalize(s);

    /* A non-grantable (plain tool-call) denial is rejected */
    int64_t bad = insert_approval(db, sid, "shell_exec", "run", "{}", "denied");
    assert(admin_grant_from_history(db, bad, "cli") == -1);

    db_close(db);
    test_db_clean(DB_PATH);
}

int main(void) {
    TEST_INIT();
    printf("test_admin_api:\n");
    RUN(test_key_env_name);
    RUN(test_set_key_known_provider);
    RUN(test_set_key_adopts_blank_env_name);
    RUN(test_set_key_custom);
    RUN(test_grant_from_history_provenance);
    RUN(test_list_pending_approvals);
    RUN(test_grant_from_history);
    printf("all admin_api tests passed\n");
    return 0;
}
