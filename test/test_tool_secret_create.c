#define _POSIX_C_SOURCE 200809L
#include "tool_secret_create.h"
#include "db.h"
#include "secret.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_tool_secret_create.db"

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

static void test_register(void) {
    sqlite3 *db = fresh_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolSecretCreateCtx ctx = { .db = db };
    assert(tool_secret_create_register(&reg, &ctx) == 0);
    ToolEntry *e = tools_lookup(&reg, "secret_create");
    assert(e && strcmp(e->name, "secret_create") == 0);
    tools_free(&reg);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: register\n");
}

static void test_creates_encrypted_row_and_hides_value(void) {
    sqlite3 *db = fresh_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolSecretCreateCtx ctx = { .db = db };
    tool_secret_create_register(&reg, &ctx);
    ToolEntry *e = tools_lookup(&reg, "secret_create");

    char *r = e->handler("{\"name\":\"TRELLO_PW\"}", e->user_data);
    assert(r);
    assert(strstr(r, "{{SECRET:TRELLO_PW}}"));
    assert(db_secret_exists(db, "TRELLO_PW"));

    /* The raw row is enc:-prefixed ciphertext, never the plaintext. */
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db, "SELECT value FROM secrets WHERE name='TRELLO_PW'",
                              -1, &st, NULL) == SQLITE_OK);
    assert(sqlite3_step(st) == SQLITE_ROW);
    const char *raw = (const char *)sqlite3_column_text(st, 0);
    assert(strncmp(raw, "enc:", 4) == 0);
    sqlite3_finalize(st);

    size_t n = 0;
    ShellSecret *loaded = db_secrets_load(db, &n);
    assert(loaded && n == 1);
    assert(!strstr(r, loaded[0].value));   /* result never carries the value */
    shell_secrets_free(loaded, n);

    free(r);
    tools_free(&reg);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: creates_encrypted_row_and_hides_value\n");
}

static void test_duplicate_name_errors(void) {
    sqlite3 *db = fresh_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolSecretCreateCtx ctx = { .db = db };
    tool_secret_create_register(&reg, &ctx);
    ToolEntry *e = tools_lookup(&reg, "secret_create");

    char *r1 = e->handler("{\"name\":\"DUP\"}", e->user_data);
    assert(r1 && !strstr(r1, "error"));
    free(r1);
    char *r2 = e->handler("{\"name\":\"DUP\"}", e->user_data);
    assert(r2 && strstr(r2, "error") && strstr(r2, "already exists"));
    free(r2);

    tools_free(&reg);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: duplicate_name_errors\n");
}

static void test_bad_name_errors(void) {
    sqlite3 *db = fresh_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolSecretCreateCtx ctx = { .db = db };
    tool_secret_create_register(&reg, &ctx);
    ToolEntry *e = tools_lookup(&reg, "secret_create");

    char *r = e->handler("{\"name\":\"lowercase\"}", e->user_data);
    assert(r && strstr(r, "error"));
    free(r);

    tools_free(&reg);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: bad_name_errors\n");
}

static void test_charset_and_length(void) {
    sqlite3 *db = fresh_db();
    ToolRegistry reg;
    tools_init(&reg);
    ToolSecretCreateCtx ctx = { .db = db };
    tool_secret_create_register(&reg, &ctx);
    ToolEntry *e = tools_lookup(&reg, "secret_create");

    char *r = e->handler("{\"name\":\"HEXY\",\"length\":16,\"charset\":\"hex\"}", e->user_data);
    assert(r && strstr(r, "16 chars") && strstr(r, "hex"));
    free(r);
    size_t n = 0;
    ShellSecret *loaded = db_secrets_load(db, &n);
    assert(loaded && n == 1 && strlen(loaded[0].value) == 16);
    for (size_t i = 0; i < strlen(loaded[0].value); i++)
        assert(strchr("0123456789abcdef", loaded[0].value[i]));
    shell_secrets_free(loaded, n);

    /* length clamps to [8,128] */
    char *r2 = e->handler("{\"name\":\"CLAMPED\",\"length\":4}", e->user_data);
    assert(r2 && strstr(r2, "8 chars"));
    free(r2);

    char *r3 = e->handler("{\"name\":\"BADCHARSET\",\"charset\":\"weird\"}", e->user_data);
    assert(r3 && strstr(r3, "error"));
    free(r3);

    tools_free(&reg);
    db_wipe_secret_key();
    sqlite3_close(db);
    printf("  PASS: charset_and_length\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_tool_secret_create:\n");
    test_register();
    test_creates_encrypted_row_and_hides_value();
    test_duplicate_name_errors();
    test_bad_name_errors();
    test_charset_and_length();
    clean_db();
    printf("  all tests passed\n");
    return 0;
}
