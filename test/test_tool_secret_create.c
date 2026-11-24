#define _POSIX_C_SOURCE 200809L
#include "tool_secret_create.h"
#include "db.h"
#include "tool_shell.h"   /* shell_secrets_free */
#include "secret.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/cclaw_test_tool_secret_create.db"

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
    test_seed_agent(db, "TestAgent");   /* the capturing / minting agent */
    return db;
}

typedef struct { sqlite3 *db; ToolRegistry reg; ToolSecretCreateCtx ctx; ToolEntry *e; } Fx;

static void fx_open(Fx *f) {
    f->db = fresh_db();
    tools_init(&f->reg);
    f->ctx = (ToolSecretCreateCtx){ .db = f->db, .agent_name = "TestAgent" };
    assert(tool_secret_create_register(&f->reg, &f->ctx) == 0);
    f->e = tools_lookup(&f->reg, "secret_create");
    assert(f->e);
}

static void fx_close(Fx *f) {
    tools_free(&f->reg);
    db_wipe_secret_key();
    sqlite3_close(f->db);
}

static void test_creates_encrypted_row_and_hides_value(void) {
    Fx f; fx_open(&f);
    sqlite3 *db = f.db;
    ToolEntry *e = f.e;

    char *r = e->handler("{\"name\":\"TRELLO_PW\"}", e->user_data, &(int){0});
    assert(r);
    assert(strstr(r, "{{SECRET:TRELLO_PW}}"));
    assert(db_secret_exists(db, "TRELLO_PW"));
    /* Granted to the minting agent, in the same transaction. */
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM grants WHERE kind='secret'"
        " AND value='TRELLO_PW' AND agent_name='TestAgent' AND source='capture'",
        0, -1) == 1);

    /* The raw row is enc:-prefixed ciphertext, never the plaintext. */
    char *raw = test_qs(db, "SELECT value FROM secrets WHERE name='TRELLO_PW'");
    assert(raw && strncmp(raw, "enc:", 4) == 0);
    free(raw);

    size_t n = 0;
    ShellSecret *loaded = db_secrets_load(db, &n);
    assert(loaded && n == 1);
    assert(!strstr(r, loaded[0].value));   /* result never carries the value */
    shell_secrets_free(loaded, n);

    free(r);
    fx_close(&f);
}

static void test_duplicate_name_errors(void) {
    Fx f; fx_open(&f);
    ToolEntry *e = f.e;

    char *r1 = e->handler("{\"name\":\"DUP\"}", e->user_data, &(int){0});
    assert(r1 && !strstr(r1, "error"));
    free(r1);
    char *r2 = e->handler("{\"name\":\"DUP\"}", e->user_data, &(int){0});
    assert(r2 && strstr(r2, "error") && strstr(r2, "already exists"));
    free(r2);

    fx_close(&f);
}

static void test_bad_name_errors(void) {
    Fx f; fx_open(&f);
    ToolEntry *e = f.e;

    char *r = e->handler("{\"name\":\"lowercase\"}", e->user_data, &(int){0});
    assert(r && strstr(r, "error"));
    free(r);

    fx_close(&f);
}

static void test_charset_and_length(void) {
    Fx f; fx_open(&f);
    sqlite3 *db = f.db;
    ToolEntry *e = f.e;

    char *r = e->handler("{\"name\":\"HEXY\",\"length\":16,\"charset\":\"hex\"}", e->user_data, &(int){0});
    assert(r && strstr(r, "16 chars") && strstr(r, "hex"));
    free(r);
    size_t n = 0;
    ShellSecret *loaded = db_secrets_load(db, &n);
    assert(loaded && n == 1 && strlen(loaded[0].value) == 16);
    for (size_t i = 0; i < strlen(loaded[0].value); i++)
        assert(strchr("0123456789abcdef", loaded[0].value[i]));
    shell_secrets_free(loaded, n);

    /* length clamps to [8,128] */
    char *r2 = e->handler("{\"name\":\"CLAMPED\",\"length\":4}", e->user_data, &(int){0});
    assert(r2 && strstr(r2, "8 chars"));
    free(r2);

    char *r3 = e->handler("{\"name\":\"BADCHARSET\",\"charset\":\"weird\"}", e->user_data, &(int){0});
    assert(r3 && strstr(r3, "error"));
    free(r3);

    fx_close(&f);
}

int main(void) {
    TEST_INIT();
    printf("test_tool_secret_create:\n");
    RUN(test_creates_encrypted_row_and_hides_value);
    RUN(test_duplicate_name_errors);
    RUN(test_bad_name_errors);
    RUN(test_charset_and_length);
    clean_db();
    return 0;
}
