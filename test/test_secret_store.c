/* Secret store (specs/security.md): DB-backed secrets, encrypted at rest, and
 * the per-call snapshot that merges them with the env-collected base. */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "agent_config.h"
#include "tool_shell.h"   /* shell_secrets_free */
#include "secret.h"
#include "secret_store.h"
#include "cli_verbs.h"
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

    assert(db_secret_rm(db, "GH_TOKEN", NULL) == 0);
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

/* Removing a secret removes the name everywhere, in one transaction: the
 * stored value, its host bindings and every 'secret' grant naming it (there
 * is no foreign key to cascade from). Other secrets' rows are untouched. An
 * environment secret (no stored row) is removable the same way. */
static void test_rm_cleans_bindings_and_grants(void) {
    sqlite3 *db = fresh_db();
    test_seed_agent(db, "Ann");
    test_seed_agent(db, "Bob");
    assert(db_secret_set(db, "GONE", "value-gone-123", "operator", NULL) == 0);
    assert(db_secret_set(db, "KEPT", "value-kept-456", "operator", NULL) == 0);
    assert(db_secret_host_bind(db, "GONE", "a.example") == 0);
    assert(db_secret_host_bind(db, "GONE", "b.example") == 0);
    assert(db_secret_host_bind(db, "KEPT", "a.example") == 0);
    assert(db_secret_host_bind(db, "ENV_ONLY", "c.example") == 0);
    const char *names[] = { "GONE", "KEPT", "ENV_ONLY" };
    for (int i = 0; i < 3; i++) {
        assert(agent_config_grant(db, "Ann", "secret", names[i], 0, GRANT_ORIGIN("operator", 0)) == 0);
        assert(agent_config_grant(db, "Bob", "secret", names[i], 0, GRANT_ORIGIN("operator", 0)) == 0);
    }
    int n[3];
    assert(db_secret_rm(db, "GONE", n) == 0);
    assert(n[0] == 1 && n[1] == 2 && n[2] == 2);
    assert(!db_secret_exists(db, "GONE"));
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM secret_hosts WHERE secret_name='GONE'", 0, -1) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE value='GONE'", 0, -1) == 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM secret_hosts", 0, -1) == 2);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE kind='secret'", 0, -1) == 4);
    assert(db_secret_rm(db, "ENV_ONLY", n) == 0);
    assert(n[0] == 0 && n[1] == 1 && n[2] == 2);
    /* Inside a caller's transaction it nests (savepoint) and rolls back with it. */
    assert(sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) == SQLITE_OK);
    assert(db_secret_rm(db, "KEPT", NULL) == 0);
    assert(sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) == SQLITE_OK);
    assert(db_secret_exists(db, "KEPT"));
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE value='KEPT'", 0, -1) == 2);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: rm_cleans_bindings_and_grants\n");
}

/* The use gate's filter: only the names this agent holds a live grant for,
 * env and stored alike, expired grants excluded; entries borrow the input. */
static void test_secrets_granted_filter(void) {
    sqlite3 *db = fresh_db();
    test_seed_agent(db, "Ann");
    ShellSecret all[3] = { { "A_ONE", "v1-xxxxxxxx" }, { "B_TWO", "v2-xxxxxxxx" },
                           { "C_THREE", "v3-xxxxxxxx" } };
    size_t n = 9;
    assert(secrets_granted(db, "Ann", all, 3, &n) == NULL && n == 0);
    assert(agent_config_grant(db, "Ann", "secret", "B_TWO", 0, GRANT_ORIGIN("operator", 0)) == 0);
    assert(agent_config_grant(db, "Ann", "secret", "C_THREE", 0, GRANT_ORIGIN("operator", 0)) == 0);
    sqlite3_exec(db, "UPDATE grants SET expires_at=unixepoch()-1 WHERE value='C_THREE'",
                 NULL, NULL, NULL);
    ShellSecret *ok = secrets_granted(db, "Ann", all, 3, &n);
    assert(ok && n == 1 && ok[0].name == all[1].name && ok[0].value == all[1].value);
    free(ok);
    assert(secrets_granted(db, NULL, all, 3, &n) == NULL && n == 0);
    assert(secret_granted(db, "Ann", "B_TWO") == 1 && secret_granted(db, "Ann", "A_ONE") == 0);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: secrets_granted_filter\n");
}

/* `cclaw secret set` with a provider's key name stores a system secret, the
 * only scope the daemon reads provider keys at, on a database that has never
 * been run (no providers row yet). Any other name is an agent secret. */
static void test_verb_set_provider_key_is_system(void) {
    clean_db();
    setenv("CCLAW_DB_PATH", DB_PATH, 1);
    char *a1[] = { "cclaw", "secret", "set", "OPENROUTER_API_KEY", "sk-or-test", NULL };
    char *a2[] = { "cclaw", "secret", "set", "MY_TOKEN", "abc", NULL };
    assert(secret_main(5, a1) == 0);
    assert(secret_main(5, a2) == 0);
    unsetenv("CCLAW_DB_PATH");
    sqlite3 *db = db_open(DB_PATH);
    assert(db);
    assert(db_scalar_i64(db, "SELECT count(*) FROM secrets WHERE name='OPENROUTER_API_KEY'"
                             " AND scope='system' AND ?1", 1, 0) == 1);
    assert(db_scalar_i64(db, "SELECT count(*) FROM secrets WHERE name='MY_TOKEN'"
                             " AND scope='agent' AND ?1", 1, 0) == 1);
    sqlite3_close(db);
    printf("  PASS: verb_set_provider_key_is_system\n");
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
    test_rm_cleans_bindings_and_grants();
    test_secrets_granted_filter();
    test_verb_set_provider_key_is_system();
    clean_db();
    printf("  all tests passed\n");
    return 0;
}
