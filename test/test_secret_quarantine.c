/* DLP quarantine (specs/security.md): tool_result_postprocess_q captures a
 * scanner-detected credential into the secret store instead of shredding it. */
#define _POSIX_C_SOURCE 200809L
#include "secret_quarantine.h"
#include "db.h"
#include "secret.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_secret_quarantine.db"
#define AWS_KEY "AKIAIOSFODNN7EXAMPLE"   /* aws-access-token fixture */

static void clean_db(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
    char keypath[512];
    snprintf(keypath, sizeof(keypath), "%s", DB_PATH);
    char *slash = strrchr(keypath, '/');
    if (slash) snprintf(slash + 1, sizeof(keypath) - (size_t)(slash + 1 - keypath), ".cclaw_key");
    unlink(keypath);
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    uint8_t key[32];
    assert(secret_key_load_or_create(DB_PATH, key) == 0);
    db_set_secret_key(key);
    return db;
}

static void test_quarantine_pending_row_and_placeholder(void) {
    sqlite3 *db = fresh_db();
    char text[256];
    snprintf(text, sizeof(text), "here is my key %s ok", AWS_KEY);

    char *r = tool_result_postprocess_q(db, text, NULL, 0);
    assert(r);
    assert(!strstr(r, AWS_KEY));            /* value gone from output */
    assert(strstr(r, "{{SECRET:PENDING_AWS_ACCESS_TOKEN_0}}"));

    size_t n = 0;
    ShellSecret *loaded = db_secrets_load(db, &n);
    assert(loaded && n == 1);
    assert(strcmp(loaded[0].name, "PENDING_AWS_ACCESS_TOKEN_0") == 0);
    assert(strcmp(loaded[0].value, AWS_KEY) == 0);
    shell_secrets_free(loaded, n);
    assert(db_secret_pending_count(db) == 1);

    free(r);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: quarantine_pending_row_and_placeholder\n");
}

static void test_dedup_same_value_within_call(void) {
    sqlite3 *db = fresh_db();
    char text[256];
    snprintf(text, sizeof(text), "%s appears twice: %s", AWS_KEY, AWS_KEY);

    char *r = tool_result_postprocess_q(db, text, NULL, 0);
    assert(r);
    assert(db_secret_pending_count(db) == 1);   /* one row, not two */
    assert(!strstr(r, AWS_KEY));

    free(r);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: dedup_same_value_within_call\n");
}

static void test_dedup_reuses_existing_secret_name(void) {
    sqlite3 *db = fresh_db();
    assert(db_secret_set(db, "MY_AWS_KEY", AWS_KEY, "operator", "active", NULL) == 0);

    size_t n = 0;
    ShellSecret *snap = db_secrets_load(db, &n);
    assert(snap && n == 1);

    char text[256];
    snprintf(text, sizeof(text), "leaked: %s", AWS_KEY);
    char *r = tool_result_postprocess_q(db, text, snap, n);
    assert(r);
    assert(strstr(r, "{{SECRET:MY_AWS_KEY}}"));   /* reused, not a new PENDING_ name */
    assert(db_secret_pending_count(db) == 0);      /* no new row minted */

    shell_secrets_free(snap, n);
    free(r);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: dedup_reuses_existing_secret_name\n");
}

static void test_cap_falls_back_to_redact(void) {
    sqlite3 *db = fresh_db();
    for (int i = 0; i < 32; i++) {
        char name[32];
        snprintf(name, sizeof(name), "FILLER_%d", i);
        assert(db_secret_set(db, name, "x", "quarantine", "pending", NULL) == 0);
    }
    assert(db_secret_pending_count(db) == 32);

    char text[256];
    snprintf(text, sizeof(text), "over cap: %s", AWS_KEY);
    char *r = tool_result_postprocess_q(db, text, NULL, 0);
    assert(r);
    assert(!strstr(r, AWS_KEY));
    assert(!strstr(r, "{{SECRET:"));       /* fell back to the shred tag, not a placeholder */
    assert(db_secret_pending_count(db) == 32);   /* no new row */

    free(r);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: cap_falls_back_to_redact\n");
}

static void test_no_findings_returns_deinterpolated_or_null(void) {
    sqlite3 *db = fresh_db();
    char *r = tool_result_postprocess_q(db, "nothing sensitive here", NULL, 0);
    assert(r == NULL);   /* no dinterpolation, no findings: caller keeps original */
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: no_findings_returns_deinterpolated_or_null\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_secret_quarantine:\n");
    test_quarantine_pending_row_and_placeholder();
    test_dedup_same_value_within_call();
    test_dedup_reuses_existing_secret_name();
    test_cap_falls_back_to_redact();
    test_no_findings_returns_deinterpolated_or_null();
    clean_db();
    printf("  all tests passed\n");
    return 0;
}
