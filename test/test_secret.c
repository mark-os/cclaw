#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include "secret.h"
#include "db.h"
#include "test_util.h"

#define FAIL(msg) do { fprintf(stderr, "FAIL: %s\n", msg); exit(1); } while(0)

static const uint8_t TEST_KEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
    0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,
    0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};

static void test_encrypt_decrypt_roundtrip(void) {
    const char *plaintext = "sk-or-v1-secret-api-key-12345";
    char *enc = secret_encrypt(TEST_KEY, plaintext);
    assert(enc != NULL);
    assert(strncmp(enc, "enc:", 4) == 0);
    assert(strlen(enc) > 4);

    char *dec = secret_decrypt(TEST_KEY, enc);
    assert(dec != NULL);
    assert(strcmp(dec, plaintext) == 0);

    free(enc);
    free(dec);
    printf("  PASS: encrypt/decrypt roundtrip\n");
}

static void test_empty_string(void) {
    char *enc = secret_encrypt(TEST_KEY, "");
    assert(enc != NULL);
    assert(strncmp(enc, "enc:", 4) == 0);

    char *dec = secret_decrypt(TEST_KEY, enc);
    assert(dec != NULL);
    assert(strcmp(dec, "") == 0);

    free(enc);
    free(dec);
    printf("  PASS: empty string roundtrip\n");
}

static void test_wrong_key_fails(void) {
    const uint8_t wrong_key[32] = {0};
    char *enc = secret_encrypt(TEST_KEY, "secret");
    assert(enc != NULL);

    char *dec = secret_decrypt(wrong_key, enc);
    assert(dec == NULL); /* auth failure */

    free(enc);
    printf("  PASS: wrong key returns NULL\n");
}

static void test_tampered_ciphertext_fails(void) {
    char *enc = secret_encrypt(TEST_KEY, "secret");
    assert(enc != NULL);

    /* Flip a byte in the hex ciphertext */
    size_t len = strlen(enc);
    enc[len / 2] ^= 0x01;

    char *dec = secret_decrypt(TEST_KEY, enc);
    assert(dec == NULL);

    free(enc);
    printf("  PASS: tampered ciphertext returns NULL\n");
}

static void test_invalid_input(void) {
    assert(secret_decrypt(TEST_KEY, "not-enc-prefix") == NULL);
    assert(secret_decrypt(TEST_KEY, "enc:") == NULL); /* too short */
    assert(secret_decrypt(TEST_KEY, "enc:zz") == NULL); /* too short for nonce+tag */
    assert(secret_encrypt(TEST_KEY, NULL) == NULL);
    assert(secret_decrypt(TEST_KEY, NULL) == NULL);
    printf("  PASS: invalid input handled\n");
}

static void test_different_encryptions_differ(void) {
    /* Same plaintext encrypted twice should produce different ciphertext (random nonce) */
    char *enc1 = secret_encrypt(TEST_KEY, "same-value");
    char *enc2 = secret_encrypt(TEST_KEY, "same-value");
    assert(enc1 && enc2);
    assert(strcmp(enc1, enc2) != 0);

    /* Both decrypt to same value */
    char *dec1 = secret_decrypt(TEST_KEY, enc1);
    char *dec2 = secret_decrypt(TEST_KEY, enc2);
    assert(dec1 && dec2);
    assert(strcmp(dec1, "same-value") == 0);
    assert(strcmp(dec2, "same-value") == 0);

    free(enc1); free(enc2); free(dec1); free(dec2);
    printf("  PASS: different encryptions differ (random nonce)\n");
}

static void test_db_secret_roundtrip(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db != NULL);

    db_set_secret_key(TEST_KEY);

    assert(db_secret_set(db, "TEST_SECRET", "my-api-key", "operator", "system") == 0);

    /* Raw value in secrets table should be encrypted */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT value FROM secrets WHERE name='TEST_SECRET'", -1, &s, NULL);
    assert(sqlite3_step(s) == SQLITE_ROW);
    const char *raw = (const char *)sqlite3_column_text(s, 0);
    assert(raw != NULL);
    assert(strncmp(raw, "enc:", 4) == 0);
    assert(strstr(raw, "my-api-key") == NULL);
    sqlite3_finalize(s);

    /* db_secret_get_system should decrypt */
    char *dec = db_secret_get_system(db, "TEST_SECRET");
    assert(dec != NULL);
    assert(strcmp(dec, "my-api-key") == 0);
    free(dec);

    db_close(db);
    printf("  PASS: db_secret roundtrip\n");
}

static void test_key_file_create_and_load(void) {
    char tmpdir[] = "/tmp/cclaw_test_XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);

    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s/test.db", tmpdir);

    uint8_t key1[32], key2[32];
    assert(secret_key_load_or_create(db_path, key1) == 0);

    /* Second load should return same key */
    assert(secret_key_load_or_create(db_path, key2) == 0);
    assert(memcmp(key1, key2, 32) == 0);

    /* Cleanup */
    char key_path[256];
    snprintf(key_path, sizeof(key_path), "%s/.cclaw_key", tmpdir);
    unlink(key_path);
    rmdir(tmpdir);
    printf("  PASS: key file create and load\n");
}

/* Verify startup integration — load_or_create + set_key → secrets work */
static void test_startup_integration(void) {
    char tmpdir[] = "/tmp/cclaw_t172_XXXXXX";
    assert(mkdtemp(tmpdir) != NULL);

    char db_path[256];
    snprintf(db_path, sizeof(db_path), "%s/test.db", tmpdir);

    /* Simulate startup: open DB, load key, set key */
    sqlite3 *db = test_db_open(db_path);
    assert(db != NULL);

    uint8_t key[32];
    assert(secret_key_load_or_create(db_path, key) == 0);
    db_set_secret_key(key);

    /* Store and retrieve a secret */
    assert(db_secret_set(db, "PROVIDER_API_KEY", "sk-test-123", "operator", "system") == 0);
    char *val = db_secret_get_system(db, "PROVIDER_API_KEY");
    assert(val != NULL);
    assert(strcmp(val, "sk-test-123") == 0);
    free(val);

    db_close(db);

    /* Simulate restart: reopen DB, reload same key, verify secret persists */
    db = test_db_open(db_path);
    assert(db != NULL);

    uint8_t key2[32];
    assert(secret_key_load_or_create(db_path, key2) == 0);
    assert(memcmp(key, key2, 32) == 0);
    db_set_secret_key(key2);

    char *val2 = db_secret_get_system(db, "PROVIDER_API_KEY");
    assert(val2 != NULL);
    assert(strcmp(val2, "sk-test-123") == 0);
    free(val2);

    db_close(db);

    /* Cleanup */
    char key_path[256];
    snprintf(key_path, sizeof(key_path), "%s/.cclaw_key", tmpdir);
    unlink(key_path);
    unlink(db_path);
    /* Remove WAL/SHM if present */
    char wal_path[280], shm_path[280];
    snprintf(wal_path, sizeof(wal_path), "%s-wal", db_path);
    snprintf(shm_path, sizeof(shm_path), "%s-shm", db_path);
    unlink(wal_path);
    unlink(shm_path);
    rmdir(tmpdir);
    printf("  PASS: startup integration (load_or_create + set_key + persist)\n");
}

int main(void) {
    printf("test_secret:\n");
    test_encrypt_decrypt_roundtrip();
    test_empty_string();
    test_wrong_key_fails();
    test_tampered_ciphertext_fails();
    test_invalid_input();
    test_different_encryptions_differ();
    test_db_secret_roundtrip();
    test_key_file_create_and_load();
    test_startup_integration();
    printf("All secret tests passed.\n");
    return 0;
}
