/* save_secret explicit capture: extract a credential from a raw tool result,
 * store it encrypted, mask it out of the result the context sees. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "tool_shell.h"   /* shell_secrets_free */
#include "secret.h"
#include "secret_capture.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_secret_capture.db"

static void clean_db(void) {
    test_db_clean(DB_PATH);
    unlink("/tmp/.cclaw_key");
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    uint8_t key[32];
    assert(secret_key_load_or_create(DB_PATH, key) == 0);
    db_set_secret_key(key);
    test_seed_agent(db, "TestAgent");   /* the capturing / minting agent */
    return db;
}

static char *loaded_value(sqlite3 *db, const char *name) {
    size_t n = 0;
    ShellSecret *s = db_secrets_load(db, &n);
    char *out = NULL;
    for (size_t i = 0; i < n; i++)
        if (strcmp(s[i].name, name) == 0) out = strdup(s[i].value);
    shell_secrets_free(s, n);
    return out;
}

static void test_no_save_secret(void) {
    sqlite3 *db = fresh_db();
    assert(secret_capture_apply(db, "TestAgent", "{\"url\":\"https://x\"}", "hello", 0) == NULL);
    assert(secret_capture_apply(db, "TestAgent", NULL, "hello", 0) == NULL);
    assert(secret_capture_apply(db, "TestAgent", "{\"url\":\"https://x\"}", NULL, 0) == NULL);
    db_wipe_secret_key();
    sqlite3_close(db);
}

static void test_whole_result_capture(void) {
    sqlite3 *db = fresh_db();
    const char *args = "{\"command\":\"gh auth token\",\"save_secret\":\"GH_TOKEN\"}";
    char *out = secret_capture_apply(db, "TestAgent", args, "  ghp_tok9K2mX4pL7qR1w  \n", 0);
    assert(out != NULL);
    assert(strstr(out, "ghp_tok9K2mX4pL7qR1w") == NULL);   /* masked */
    assert(strstr(out, "{{SECRET:GH_TOKEN}}") != NULL);
    assert(strstr(out, "saved {{SECRET:GH_TOKEN}}") != NULL);
    char *v = loaded_value(db, "GH_TOKEN");
    assert(v && strcmp(v, "ghp_tok9K2mX4pL7qR1w") == 0);   /* trimmed */
    /* The capturing agent — and only it — may use what it captured. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE kind='secret'"
        " AND value='GH_TOKEN' AND agent_name='TestAgent' AND source='capture'"
        " AND approval_id IS NULL", 0, -1) == 1);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE value='GH_TOKEN'",
                         0, -1) == 1);
    free(v); free(out);
    db_wipe_secret_key();
    sqlite3_close(db);
}

static void test_json_path_capture(void) {
    sqlite3 *db = fresh_db();
    const char *args =
        "{\"url\":\"https://api.github.com/x\",\"save_secret\":\"API_KEY\","
        "\"save_secret_path\":\"$.token\"}";
    const char *result = "{\"id\":42,\"token\":\"tok4Xq9Lm2Rp8Vw1\",\"ok\":true}";
    char *out = secret_capture_apply(db, "TestAgent", args, result, 0);
    assert(out != NULL);
    assert(strstr(out, "tok4Xq9Lm2Rp8Vw1") == NULL);
    assert(strstr(out, "{{SECRET:API_KEY}}") != NULL);
    assert(strstr(out, "\"id\":42") != NULL);   /* rest of result intact */
    char *v = loaded_value(db, "API_KEY");
    assert(v && strcmp(v, "tok4Xq9Lm2Rp8Vw1") == 0);
    free(v); free(out);
    db_wipe_secret_key();
    sqlite3_close(db);
}

static void test_capture_errors(void) {
    sqlite3 *db = fresh_db();

    /* invalid name: nothing stored, note appended, result preserved */
    char *out = secret_capture_apply(db, "TestAgent",
        "{\"save_secret\":\"bad-name\"}", "some output", 0);
    assert(out && strstr(out, "some output") && strstr(out, "invalid name"));
    assert(!db_secret_exists(db, "bad-name"));
    free(out);

    /* existing name: refused */
    assert(db_secret_set(db, "TAKEN", "v", "operator", "agent") == 0);
    out = secret_capture_apply(db, "TestAgent", "{\"save_secret\":\"TAKEN\"}", "newvalue", 0);
    assert(out && strstr(out, "already exists"));
    char *v = loaded_value(db, "TAKEN");
    assert(v && strcmp(v, "v") == 0);   /* not clobbered */
    free(v); free(out);

    /* error result: skipped */
    /* Failure status, not the prose, is what skips capture: the message here
     * carries no "error:" prefix at all. */
    out = secret_capture_apply(db, "TestAgent", "{\"save_secret\":\"E_KEY\"}",
                               "HTTP 403 Forbidden", 1);
    assert(out && strstr(out, "skipped"));
    assert(!db_secret_exists(db, "E_KEY"));
    free(out);

    /* path into non-JSON: failure note */
    out = secret_capture_apply(db, "TestAgent",
        "{\"save_secret\":\"P_KEY\",\"save_secret_path\":\"$.token\"}",
        "<html>not json</html>", 0);
    assert(out && strstr(out, "save_secret failed"));
    assert(!db_secret_exists(db, "P_KEY"));
    free(out);

    db_wipe_secret_key();
    sqlite3_close(db);
}

int main(void) {
    TEST_INIT();
    RUN(test_no_save_secret);
    RUN(test_whole_result_capture);
    RUN(test_json_path_capture);
    RUN(test_capture_errors);
    clean_db();
    return 0;
}
