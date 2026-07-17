/* Secret store (specs/security.md): DB-backed secrets, encrypted at rest, and
 * the per-call snapshot that merges them with the env-collected base. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "tool_shell.h"   /* shell_secrets_free */
#include "secret.h"
#include "secret_store.h"
#include "validate.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_secret_store.db"

static void clean_db(void) {
    test_db_clean(DB_PATH);
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

static void test_set_load_rm_roundtrip(void) {
    sqlite3 *db = fresh_db();
    assert(db_secret_exists(db, "GH_TOKEN") == 0);
    assert(db_secret_set(db, "GH_TOKEN", "ghp_abc123", "operator", NULL) == 0);
    assert(db_secret_exists(db, "GH_TOKEN") == 1);

    size_t n = 0;
    ShellSecret *loaded = db_secrets_load(db, &n);
    assert(loaded && n == 1);
    assert(strcmp(loaded[0].name, "GH_TOKEN") == 0);
    assert(strcmp(loaded[0].value, "ghp_abc123") == 0);
    shell_secrets_free(loaded, n);

    /* Overwrite (upsert) */
    assert(db_secret_set(db, "GH_TOKEN", "ghp_new", "operator", NULL) == 0);
    loaded = db_secrets_load(db, &n);
    assert(n == 1 && strcmp(loaded[0].value, "ghp_new") == 0);
    shell_secrets_free(loaded, n);

    assert(db_secret_rm(db, "GH_TOKEN") == 0);
    assert(db_secret_exists(db, "GH_TOKEN") == 0);
    loaded = db_secrets_load(db, &n);
    assert(loaded == NULL && n == 0);

    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: set_load_rm_roundtrip\n");
}

static void test_set_fails_without_key(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    assert(db_secret_key_loaded() == 0);
    assert(db_secret_set(db, "X", "value", "operator", NULL) == -1);
    sqlite3_close(db);
    printf("  PASS: set_fails_without_key\n");
}

static void test_captured_loads(void) {
    /* A save_secret capture ('captured' source) is a first-class secret: it
     * loads into the snapshot exactly like an operator-set one. */
    sqlite3 *db = fresh_db();
    assert(db_secret_set(db, "CAPTURED_ONE", "v1", "captured", NULL) == 0);
    assert(db_secret_set(db, "ACTIVE_ONE", "v2", "operator", NULL) == 0);
    size_t n = 0;
    ShellSecret *s = db_secrets_load(db, &n);
    assert(n == 2 && s);
    shell_secrets_free(s, n);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: captured_loads\n");
}

static void test_snapshot_env_wins(void) {
    sqlite3 *db = fresh_db();
    assert(db_secret_set(db, "SHARED", "from_db", "operator", NULL) == 0);
    assert(db_secret_set(db, "DB_ONLY", "db_value", "operator", NULL) == 0);

    ShellSecret env[1] = { { .name = strdup("SHARED"), .value = strdup("from_env") } };
    size_t snap_n = 0;
    ShellSecret *snap = secrets_snapshot(db, env, 1, &snap_n);
    assert(snap && snap_n == 2);

    const char *shared_val = NULL, *db_only_val = NULL;
    for (size_t i = 0; i < snap_n; i++) {
        if (strcmp(snap[i].name, "SHARED") == 0) shared_val = snap[i].value;
        if (strcmp(snap[i].name, "DB_ONLY") == 0) db_only_val = snap[i].value;
    }
    assert(shared_val && strcmp(shared_val, "from_env") == 0);   /* env wins */
    assert(db_only_val && strcmp(db_only_val, "db_value") == 0);

    secrets_snapshot_free(snap, snap_n);
    free(env[0].name);
    free(env[0].value);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: snapshot_env_wins\n");
}

static void test_snapshot_empty(void) {
    sqlite3 *db = fresh_db();
    size_t snap_n = 99;
    ShellSecret *snap = secrets_snapshot(db, NULL, 0, &snap_n);
    assert(snap == NULL && snap_n == 0);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: snapshot_empty\n");
}

static void test_name_validation(void) {
    assert(is_valid_secret_name("GITHUB_TOKEN"));
    assert(is_valid_secret_name("A"));
    assert(is_valid_secret_name("A1_2"));
    assert(!is_valid_secret_name("github_token"));   /* lowercase */
    assert(!is_valid_secret_name("1TOKEN"));          /* must start with letter */
    assert(!is_valid_secret_name(""));
    assert(!is_valid_secret_name(NULL));
    assert(!is_valid_secret_name("TOKEN-NAME"));      /* hyphen not allowed */
    printf("  PASS: name_validation\n");
}

int main(void) {
    TEST_INIT();
    printf("test_secret_store:\n");
    test_set_load_rm_roundtrip();
    test_set_fails_without_key();
    test_captured_loads();
    test_snapshot_env_wins();
    test_snapshot_empty();
    test_name_validation();
    clean_db();
    printf("  all tests passed\n");
    return 0;
}
