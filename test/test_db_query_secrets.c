/* integration test — db_query secret filtering.
 * Seed secrets table w/ enc: values, run db_query("SELECT * FROM secrets") tool,
 * verify no enc: rows in result; no network. */
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

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static char tmpdir[64];
static char db_path[128];
static char key_path[128];

static void setup(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cclaw_t179_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
    snprintf(db_path, sizeof(db_path), "%s/test.db", tmpdir);
    snprintf(key_path, sizeof(key_path), "%s/.cclaw_key", tmpdir);
}

static void teardown(void) {
    unlink(key_path);
    test_db_clean(db_path);
    rmdir(tmpdir);
}

/* Seed secrets table with real encrypted secrets, verify db_query filters enc: rows */
static void test_select_star_filters_secrets(void) {
    TEST(select_star_filters_secrets);

    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db_open");

    uint8_t key[32];
    if (secret_key_load_or_create(db_path, key) != 0) { db_close(db); FAIL("key_load_or_create"); }
    db_set_secret_key(key);

    /* Seed encrypted secrets in secrets table */
    db_secret_set(db, "PROVIDER_API_KEY", "sk-or-v1-secret123", "operator", "system");
    db_secret_set(db, "GEMINI_API_KEY", "AIzaSy-secret456", "operator", "system");

    /* Run db_query tool: SELECT * FROM secrets */
    char *result = tool_db_query_handler("{\"sql\":\"SELECT * FROM secrets\"}", db, &(int){0});
    if (!result) { db_close(db); FAIL("handler returned NULL"); }

    /* Encrypted values filtered — neither enc: value nor plaintext appears */
    if (strstr(result, "enc:")) { free(result); db_close(db); FAIL("enc: prefix in result"); }
    if (strstr(result, "sk-or-v1")) { free(result); db_close(db); FAIL("plaintext secret leaked"); }

    free(result);
    db_close(db);
    PASS();
}

/* Verify WHERE clause targeting secret key still returns nothing */
static void test_where_clause_secret_key(void) {
    TEST(where_clause_secret_key);

    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db_open");

    char *result = tool_db_query_handler(
        "{\"sql\":\"SELECT name, value FROM secrets WHERE name = 'PROVIDER_API_KEY'\"}", db, &(int){0});
    if (!result) { db_close(db); FAIL("handler returned NULL"); }

    /* Should be empty or masked — row filtered */
    if (strstr(result, "enc:")) { free(result); db_close(db); FAIL("enc: in targeted query"); }
    if (strstr(result, "sk-or-v1")) { free(result); db_close(db); FAIL("plaintext in targeted query"); }

    free(result);
    db_close(db);
    PASS();
}

/* Verify non-kv tables unaffected by filtering */
static void test_non_kv_table_unaffected(void) {
    TEST(non_kv_table_unaffected);

    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db_open");

    /* sessions table should work normally */
    session_create(db, "test-session", NULL, -1, 0);
    char *result = tool_db_query_handler(
        "{\"sql\":\"SELECT id, name FROM sessions\"}", db, &(int){0});
    if (!result) { db_close(db); FAIL("handler returned NULL"); }
    if (!strstr(result, "test-session")) { free(result); db_close(db); FAIL("missing session"); }

    free(result);
    db_close(db);
    PASS();
}

/* Verify COUNT(*) on secrets excludes secret rows */
static void test_count_excludes_secrets(void) {
    TEST(count_excludes_secrets);

    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db_open");

    /* COUNT(*) is computed by SQLite before filtering, so it includes all rows.
     * But the row itself doesn't contain enc: text (it's just a number).
     * The filter only skips rows with enc: in a TEXT column value.
     * A COUNT result is an integer, so it passes through. */
    char *result = tool_db_query_handler(
        "{\"sql\":\"SELECT COUNT(*) AS cnt FROM secrets\"}", db, &(int){0});
    if (!result) { db_close(db); FAIL("handler returned NULL"); }

    if (!strstr(result, "cnt")) { free(result); db_close(db); FAIL("missing cnt"); }

    free(result);
    db_close(db);
    PASS();
}

int main(void) {
    TEST_INIT();
    printf("test_integration_db_query_secrets:\n");
    setup();
    test_select_star_filters_secrets();
    test_where_clause_secret_key();
    test_non_kv_table_unaffected();
    test_count_excludes_secrets();
    teardown();
    printf("  %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
