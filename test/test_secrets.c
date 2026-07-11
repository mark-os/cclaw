/* integration test — secrets round-trip.
 * db_secret_set stores enc: prefix; db_secret_get_system returns plaintext;
 * delete key file → decrypt fails gracefully; no network.
 * Cites the AEAD encryption design. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "db.h"
#include "test_util.h"
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
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cclaw_t176_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
    snprintf(db_path, sizeof(db_path), "%s/test.db", tmpdir);
    snprintf(key_path, sizeof(key_path), "%s/.cclaw_key", tmpdir);
}

static void teardown(void) {
    unlink(key_path);
    unlink(db_path);
    char wal[160], shm[160];
    snprintf(wal, sizeof(wal), "%s-wal", db_path);
    snprintf(shm, sizeof(shm), "%s-shm", db_path);
    unlink(wal);
    unlink(shm);
    rmdir(tmpdir);
}

/* Store secret → raw value has enc: prefix → decrypt returns plaintext */
static void test_roundtrip(void) {
    TEST(roundtrip);

    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db_open");

    uint8_t key[32];
    if (secret_key_load_or_create(db_path, key) != 0) { db_close(db); FAIL("key_load_or_create"); }
    db_set_secret_key(key);

    /* Store */
    if (db_secret_set(db, "PROVIDER_API_KEY", "sk-or-v1-realkey", "operator", "system") != 0) { db_close(db); FAIL("db_secret_set"); }

    /* Raw value must be encrypted */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT value FROM secrets WHERE name='PROVIDER_API_KEY'", -1, &s, NULL);
    if (sqlite3_step(s) != SQLITE_ROW) { sqlite3_finalize(s); db_close(db); FAIL("no row"); }
    const char *raw = (const char *)sqlite3_column_text(s, 0);
    if (!raw) { sqlite3_finalize(s); db_close(db); FAIL("raw NULL"); }
    if (strncmp(raw, "enc:", 4) != 0) { sqlite3_finalize(s); db_close(db); FAIL("missing enc: prefix"); }
    if (strstr(raw, "sk-or-v1-realkey") != NULL) { sqlite3_finalize(s); db_close(db); FAIL("stored plaintext!"); }
    sqlite3_finalize(s);

    /* Decrypt returns original */
    char *dec = db_secret_get_system(db, "PROVIDER_API_KEY");
    if (!dec) { db_close(db); FAIL("db_secret_get_system NULL"); }
    if (strcmp(dec, "sk-or-v1-realkey") != 0) { free(dec); db_close(db); FAIL("decrypt mismatch"); }
    free(dec);

    db_close(db);
    PASS();
}

/* Delete key file → decrypt fails gracefully (returns NULL, no crash) */
static void test_deleted_key_file(void) {
    TEST(deleted_key_file_graceful_fail);

    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db_open");

    /* Key was created in previous test; store a secret */
    uint8_t key[32];
    if (secret_key_load_or_create(db_path, key) != 0) { db_close(db); FAIL("key_load_or_create"); }
    db_set_secret_key(key);
    db_secret_set(db, "TEST_KEY", "secret-value", "operator", "system");
    db_close(db);

    /* Delete the key file */
    unlink(key_path);

    /* Reopen DB — load key should create a NEW key (different from original) */
    db = test_db_open(db_path);
    if (!db) FAIL("db_open 2");

    uint8_t new_key[32];
    if (secret_key_load_or_create(db_path, new_key) != 0) { db_close(db); FAIL("key_load_or_create 2"); }
    db_set_secret_key(new_key);

    /* Keys differ (new random key generated) */
    if (memcmp(key, new_key, 32) == 0) { db_close(db); FAIL("key should differ after recreate"); }

    /* Decrypt with wrong key fails gracefully (NULL, no crash) */
    char *dec = db_secret_get_system(db, "TEST_KEY");
    if (dec != NULL) { free(dec); db_close(db); FAIL("expected NULL on wrong key"); }

    db_close(db);
    PASS();
}

/* Multiple secrets stored independently */
static void test_multiple_secrets(void) {
    TEST(multiple_secrets);

    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db_open");

    uint8_t key[32];
    if (secret_key_load_or_create(db_path, key) != 0) { db_close(db); FAIL("key_load_or_create"); }
    db_set_secret_key(key);

    db_secret_set(db, "KEY_ONE", "value-one", "operator", "system");
    db_secret_set(db, "KEY_TWO", "value-two", "operator", "system");
    db_secret_set(db, "KEY_THREE", "value-three", "operator", "system");

    char *v1 = db_secret_get_system(db, "KEY_ONE");
    char *v2 = db_secret_get_system(db, "KEY_TWO");
    char *v3 = db_secret_get_system(db, "KEY_THREE");

    if (!v1 || strcmp(v1, "value-one") != 0) { free(v1); free(v2); free(v3); db_close(db); FAIL("KEY_ONE"); }
    if (!v2 || strcmp(v2, "value-two") != 0) { free(v1); free(v2); free(v3); db_close(db); FAIL("KEY_TWO"); }
    if (!v3 || strcmp(v3, "value-three") != 0) { free(v1); free(v2); free(v3); db_close(db); FAIL("KEY_THREE"); }

    free(v1); free(v2); free(v3);
    db_close(db);
    PASS();
}

/* Overwrite secret → new value decrypts correctly */
static void test_overwrite_secret(void) {
    TEST(overwrite_secret);

    sqlite3 *db = test_db_open(db_path);
    if (!db) FAIL("db_open");

    uint8_t key[32];
    if (secret_key_load_or_create(db_path, key) != 0) { db_close(db); FAIL("key_load_or_create"); }
    db_set_secret_key(key);

    db_secret_set(db, "OVERWRITE_KEY", "original", "operator", "system");
    db_secret_set(db, "OVERWRITE_KEY", "updated", "operator", "system");

    char *dec = db_secret_get_system(db, "OVERWRITE_KEY");
    if (!dec || strcmp(dec, "updated") != 0) { free(dec); db_close(db); FAIL("overwrite mismatch"); }
    free(dec);

    db_close(db);
    PASS();
}

int main(void) {
    printf("test_integration_secrets:\n");
    setup();
    test_roundtrip();
    test_deleted_key_file();
    test_multiple_secrets();
    test_overwrite_secret();
    teardown();
    printf("  %d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
