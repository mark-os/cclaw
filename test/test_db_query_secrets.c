/* db_query secret filtering: encrypted secrets rows never reach the result,
 * neither as enc: ciphertext nor as plaintext. No network. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "db.h"
#include "test_util.h"
#include "tool_db_query.h"
#include "secret.h"

static char tmpdir[64];
static char db_path[128];
static char key_path[128];

static void assert_no_secret(const char *sql_json, sqlite3 *db) {
    char *result = tool_db_query_handler(sql_json, db, &(int){0});
    assert(result);
    assert(!strstr(result, "enc:"));
    assert(!strstr(result, "sk-or-v1"));
    free(result);
}

static void test_select_star_filters_secrets(void) {
    sqlite3 *db = test_db_open(db_path);
    assert(db);
    uint8_t key[32];
    assert(secret_key_load_or_create(db_path, key) == 0);
    db_set_secret_key(key);
    db_secret_set(db, "PROVIDER_API_KEY", "sk-or-v1-secret123", "operator", "system");
    db_secret_set(db, "GEMINI_API_KEY", "AIzaSy-secret456", "operator", "system");
    assert_no_secret("{\"sql\":\"SELECT * FROM secrets\"}", db);
    db_close(db);
}

static void test_where_clause_secret_key(void) {
    sqlite3 *db = test_db_open(db_path);
    assert(db);
    assert_no_secret("{\"sql\":\"SELECT name, value FROM secrets"
                     " WHERE name = 'PROVIDER_API_KEY'\"}", db);
    db_close(db);
}

/* The filter must not blank out ordinary tables. */
static void test_non_kv_table_unaffected(void) {
    sqlite3 *db = test_db_open(db_path);
    assert(db);
    session_create(db, "test-session", NULL, -1, 0);
    char *result = tool_db_query_handler(
        "{\"sql\":\"SELECT id, name FROM sessions\"}", db, &(int){0});
    assert(result && strstr(result, "test-session"));
    free(result);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    printf("test_integration_db_query_secrets:\n");
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cclaw_t179_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
    snprintf(db_path, sizeof(db_path), "%s/test.db", tmpdir);
    snprintf(key_path, sizeof(key_path), "%s/.cclaw_key", tmpdir);
    RUN(test_select_star_filters_secrets);
    RUN(test_where_clause_secret_key);
    RUN(test_non_kv_table_unaffected);
    unlink(key_path);
    test_db_clean(db_path);
    rmdir(tmpdir);
    return 0;
}
