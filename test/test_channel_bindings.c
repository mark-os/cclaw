#include "db.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_DB "test_channel_bindings.db"

static void test_get_unbound(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    char *agent = db_channel_binding_get(db, "telegram", "12345");
    assert(agent == NULL);
    db_close(db);
}

static void test_set_and_get(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    assert(db_channel_binding_set(db, "telegram", "99", "myagent") == 0);
    char *agent = db_channel_binding_get(db, "telegram", "99");
    assert(agent && strcmp(agent, "myagent") == 0);
    free(agent);
    db_close(db);
}

static void test_overwrite_binding(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    assert(db_channel_binding_set(db, "telegram", "200", "agent_a") == 0);
    assert(db_channel_binding_set(db, "telegram", "200", "agent_b") == 0);
    char *agent = db_channel_binding_get(db, "telegram", "200");
    assert(agent && strcmp(agent, "agent_b") == 0);
    free(agent);
    db_close(db);
}

static void test_different_channels(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    assert(db_channel_binding_set(db, "telegram", "1", "tg_agent") == 0);
    assert(db_channel_binding_set(db, "cli", "1", "cli_agent") == 0);
    char *a1 = db_channel_binding_get(db, "telegram", "1");
    char *a2 = db_channel_binding_get(db, "cli", "1");
    assert(a1 && strcmp(a1, "tg_agent") == 0);
    assert(a2 && strcmp(a2, "cli_agent") == 0);
    free(a1);
    free(a2);
    db_close(db);
}

static void test_session_uses_binding(void) {
    sqlite3 *db = db_open(TEST_DB);
    assert(db);
    /* Set binding, create session with that agent */
    assert(db_channel_binding_set(db, "telegram", "555", "bound_agent") == 0);
    char *agent = db_channel_binding_get(db, "telegram", "555");
    assert(agent);
    int64_t sid = session_create(db, "test_sess", agent, -1, 0);
    assert(sid > 0);
    free(agent);
    /* Verify session has correct agent_name */
    char *got = session_get_agent_name(db, sid);
    assert(got && strcmp(got, "bound_agent") == 0);
    free(got);
    db_close(db);
}

int main(void) {
    unlink(TEST_DB);
    printf("test_get_unbound...");
    test_get_unbound();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_set_and_get...");
    test_set_and_get();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_overwrite_binding...");
    test_overwrite_binding();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_different_channels...");
    test_different_channels();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("test_session_uses_binding...");
    test_session_uses_binding();
    printf(" OK\n");

    unlink(TEST_DB);
    printf("all channel_bindings tests passed\n");
    return 0;
}
