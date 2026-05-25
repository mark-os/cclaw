#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#define TEST_DB "/tmp/test_approvals.sqlite"

static sqlite3 *setup(void) {
    unlink(TEST_DB);
    return db_open(TEST_DB);
}

static void teardown(sqlite3 *db) {
    db_close(db);
    unlink(TEST_DB);
}

static void test_insert_and_get(void) {
    sqlite3 *db = setup();
    int64_t id = approval_insert(db, 1, "coder", "whitelist_host",
                                 "{\"host\":\"api.example.com\"}");
    assert(id > 0);

    Approval *a = approval_get(db, id);
    assert(a != NULL);
    assert(a->id == id);
    assert(a->session_id == 1);
    assert(strcmp(a->agent_name, "coder") == 0);
    assert(strcmp(a->type, "whitelist_host") == 0);
    assert(strcmp(a->payload, "{\"host\":\"api.example.com\"}") == 0);
    assert(strcmp(a->status, "pending") == 0);
    assert(a->admin_chat_id == 0);
    assert(a->resolved_at == 0);
    approval_free(a);

    teardown(db);
    printf("  PASS test_insert_and_get\n");
}

static void test_list_pending(void) {
    sqlite3 *db = setup();
    approval_insert(db, 1, "bot", "create_agent", "{\"name\":\"helper\"}");
    approval_insert(db, 2, "bot", "model_change", "{\"model\":\"gpt-5\"}");

    int count = 0;
    Approval *list = approval_list_pending(db, &count);
    assert(count == 2);
    assert(strcmp(list[0].type, "create_agent") == 0);
    assert(strcmp(list[1].type, "model_change") == 0);
    approval_list_free(list, count);

    teardown(db);
    printf("  PASS test_list_pending\n");
}

static void test_resolve_approve(void) {
    sqlite3 *db = setup();
    int64_t id = approval_insert(db, 1, "coder", "tool_enable", "{\"tool\":\"shell\"}");

    int rc = approval_resolve(db, id, "approved", 12345);
    assert(rc == 0);

    Approval *a = approval_get(db, id);
    assert(strcmp(a->status, "approved") == 0);
    assert(a->admin_chat_id == 12345);
    assert(a->resolved_at > 0);
    approval_free(a);

    /* No longer in pending list */
    int count = 0;
    Approval *list = approval_list_pending(db, &count);
    assert(count == 0);
    assert(list == NULL);

    teardown(db);
    printf("  PASS test_resolve_approve\n");
}

static void test_resolve_deny(void) {
    sqlite3 *db = setup();
    int64_t id = approval_insert(db, 1, "coder", "whitelist_host", "{\"host\":\"evil.com\"}");

    int rc = approval_resolve(db, id, "denied", 99);
    assert(rc == 0);

    Approval *a = approval_get(db, id);
    assert(strcmp(a->status, "denied") == 0);
    approval_free(a);

    teardown(db);
    printf("  PASS test_resolve_deny\n");
}

static void test_resolve_already_resolved(void) {
    sqlite3 *db = setup();
    int64_t id = approval_insert(db, 1, "coder", "whitelist_host", "{}");
    approval_resolve(db, id, "approved", 1);

    /* Second resolve fails (WHERE status='pending') */
    int rc = approval_resolve(db, id, "denied", 2);
    assert(rc == -1);

    teardown(db);
    printf("  PASS test_resolve_already_resolved\n");
}

int main(void) {
    printf("test_approvals:\n");
    test_insert_and_get();
    test_list_pending();
    test_resolve_approve();
    test_resolve_deny();
    test_resolve_already_resolved();
    printf("All approvals tests passed.\n");
    return 0;
}
