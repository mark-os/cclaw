#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "agent_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>

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
    int64_t id = approval_insert(db, 1, "coder", NULL, "whitelist_host",
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
    approval_insert(db, 1, "bot", NULL, "create_agent", "{\"name\":\"helper\"}");
    approval_insert(db, 2, "bot", NULL, "model_change", "{\"model\":\"gpt-5\"}");

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
    int64_t id = approval_insert(db, 1, "coder", NULL, "tool_enable", "{\"tool\":\"shell\"}");

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
    int64_t id = approval_insert(db, 1, "coder", NULL, "whitelist_host", "{\"host\":\"evil.com\"}");

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
    int64_t id = approval_insert(db, 1, "coder", NULL, "whitelist_host", "{}");
    approval_resolve(db, id, "approved", 1);

    /* Second resolve fails (WHERE status='pending') */
    int rc = approval_resolve(db, id, "denied", 2);
    assert(rc == -1);

    teardown(db);
    printf("  PASS test_resolve_already_resolved\n");
}

/* T148: test unnotified listing and mark_notified */
static void test_list_unnotified(void) {
    sqlite3 *db = setup();
    int64_t id1 = approval_insert(db, 1, "bot", NULL, "whitelist_host", "{\"host\":\"a.com\"}");
    int64_t id2 = approval_insert(db, 2, "bot", NULL, "model_change", "{\"model\":\"x\"}");

    /* Both should be unnotified initially */
    int count = 0;
    Approval *list = approval_list_unnotified(db, &count);
    assert(count == 2);
    assert(list[0].id == id1);
    assert(list[1].id == id2);
    approval_list_free(list, count);

    /* Mark first as notified */
    int rc = approval_mark_notified(db, id1);
    assert(rc == 0);

    /* Only second should remain unnotified */
    list = approval_list_unnotified(db, &count);
    assert(count == 1);
    assert(list[0].id == id2);
    approval_list_free(list, count);

    /* Mark second as notified */
    approval_mark_notified(db, id2);
    list = approval_list_unnotified(db, &count);
    assert(count == 0);
    assert(list == NULL);

    teardown(db);
    printf("  PASS test_list_unnotified\n");
}

/* T149: approval resolve posts result to agent inbox + transitions session state */
static void test_approval_posts_to_inbox(void) {
    sqlite3 *db = setup();

    /* Create a session in "waiting" state */
    int64_t sid = session_create(db, NULL, NULL, -1, 0);
    assert(sid > 0);
    assert(session_set_state(db, sid, "running") == 0);
    assert(session_set_state(db, sid, "waiting") == 0);

    /* Insert approval tied to that session */
    int64_t aid = approval_insert(db, sid, "coder", NULL, "whitelist_host",
                                  "{\"host\":\"api.example.com\"}");
    assert(aid > 0);

    /* Resolve it (simulating what handle_approval_callback does) */
    int rc = approval_resolve(db, aid, "approved", 12345);
    assert(rc == 0);

    /* Post to inbox (same as handle_approval_callback) */
    int64_t iid = inbox_insert(db, sid, "approval", "Approval #1 approved: whitelist_host");
    assert(iid > 0);

    /* Transition waiting→idle */
    const char *wake_sql =
        "UPDATE sessions SET state='idle' WHERE id=? AND state='waiting';";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, wake_sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Verify inbox has the message */
    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 1);
    assert(strstr(items[0].payload, "approved") != NULL);
    assert(strcmp(items[0].source, "approval") == 0);
    inbox_items_free(items, count);

    /* Verify session is now idle */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?;", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "idle") == 0);
    sqlite3_finalize(s);

    teardown(db);
    printf("  PASS test_approval_posts_to_inbox\n");
}

/* T149: denied approval posts denial to inbox */
static void test_approval_deny_posts_to_inbox(void) {
    sqlite3 *db = setup();

    int64_t sid = session_create(db, NULL, NULL, -1, 0);
    assert(sid > 0);
    assert(session_set_state(db, sid, "running") == 0);
    assert(session_set_state(db, sid, "waiting") == 0);

    int64_t aid = approval_insert(db, sid, "coder", NULL, "model_change",
                                  "{\"model\":\"gpt-5\"}");
    assert(aid > 0);

    int rc = approval_resolve(db, aid, "denied", 99);
    assert(rc == 0);

    inbox_insert(db, sid, "approval", "Approval denied: model_change");

    /* Transition waiting→idle */
    const char *wake_sql =
        "UPDATE sessions SET state='idle' WHERE id=? AND state='waiting';";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, wake_sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    int count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &count);
    assert(count == 1);
    assert(strstr(items[0].payload, "denied") != NULL);
    inbox_items_free(items, count);

    /* Verify session is now idle */
    sqlite3_stmt *s;
    sqlite3_prepare_v2(db, "SELECT state FROM sessions WHERE id=?;", -1, &s, NULL);
    sqlite3_bind_int64(s, 1, sid);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "idle") == 0);
    sqlite3_finalize(s);

    teardown(db);
    printf("  PASS test_approval_deny_posts_to_inbox\n");
}

/* T150: create_agent approval creates agent on disk + seeds DB */
static void test_create_agent_approval(void) {
    sqlite3 *db = setup();
    const char *agents_dir = "/tmp/test_create_agent_dir";
    system("rm -rf /tmp/test_create_agent_dir");
    mkdir(agents_dir, 0755);

    const char *payload = "{\"name\":\"helper\",\"model\":\"gpt-4\","
        "\"system_prompt\":\"You are a helper agent.\","
        "\"tools\":[\"file_read\",\"shell_exec\"],"
        "\"allowed_hosts\":[\"api.example.com\"]}";

    int rc = agent_config_create(agents_dir, db, payload);
    assert(rc == 0);

    /* T196: Verify config was written to daemon.db agent_config table */
    AgentConfig *ac = agent_config_load_db(db, "helper");
    assert(ac != NULL);
    assert(strcmp(ac->model, "gpt-4") == 0);
    assert(ac->tool_count == 2);
    assert(strcmp(ac->tools[0], "file_read") == 0);
    assert(ac->allowed_hosts_count == 1);
    assert(strcmp(ac->allowed_hosts[0], "api.example.com") == 0);
    agent_config_free(ac);

    /* Verify system.md was written */
    char path[256];
    snprintf(path, sizeof(path), "%s/helper/system.md", agents_dir);
    FILE *f = fopen(path, "r");
    assert(f != NULL);
    char buf[2048];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    assert(strcmp(buf, "You are a helper agent.") == 0);

    /* Verify DB agents table was seeded */
    AgentRow *row = db_agent_get(db, "helper");
    assert(row != NULL);
    assert(strcmp(row->name, "helper") == 0);
    assert(row->system_prompt != NULL);
    assert(strstr(row->system_prompt, "helper agent") != NULL);
    agent_row_free(row);

    system("rm -rf /tmp/test_create_agent_dir");
    teardown(db);
    printf("  PASS test_create_agent_approval\n");
}

/* T150: create_agent rejects invalid names */
static void test_create_agent_invalid_name(void) {
    sqlite3 *db = setup();
    const char *agents_dir = "/tmp/test_create_agent_dir2";
    system("rm -rf /tmp/test_create_agent_dir2");
    mkdir(agents_dir, 0755);

    /* Path traversal */
    assert(agent_config_create(agents_dir, db, "{\"name\":\"../evil\"}") == -1);
    /* Slash in name */
    assert(agent_config_create(agents_dir, db, "{\"name\":\"a/b\"}") == -1);
    /* Empty name */
    assert(agent_config_create(agents_dir, db, "{\"name\":\"\"}") == -1);
    /* Missing name */
    assert(agent_config_create(agents_dir, db, "{\"model\":\"x\"}") == -1);

    system("rm -rf /tmp/test_create_agent_dir2");
    teardown(db);
    printf("  PASS test_create_agent_invalid_name\n");
}

int main(void) {
    printf("test_approvals:\n");
    test_insert_and_get();
    test_list_pending();
    test_resolve_approve();
    test_resolve_deny();
    test_resolve_already_resolved();
    test_list_unnotified();
    test_approval_posts_to_inbox();
    test_approval_deny_posts_to_inbox();
    test_create_agent_approval();
    test_create_agent_invalid_name();
    printf("All approvals tests passed.\n");
    return 0;
}
