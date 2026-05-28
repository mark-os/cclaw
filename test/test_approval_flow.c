/* T151: approval flow end-to-end
 * V54: agent requests → admin approves/denies → config updated → inbox confirmation
 * V53: unauthorized approval attempt rejected */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "tool_approval.h"
#include "tools.h"
#include "agent_config.h"
#include "telegram.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#define TEST_AGENTS_DIR "/tmp/test_approval_flow_agents"

static sqlite3 *setup(void) {
    sqlite3 *db = db_open(":memory:");
    assert(db);
    /* T196: Seed agent config in DB instead of agent.json */
    system("rm -rf " TEST_AGENTS_DIR);
    mkdir(TEST_AGENTS_DIR, 0755);
    mkdir(TEST_AGENTS_DIR "/coder", 0755);
    /* Seed allowed_hosts in DB */
    agent_config_add_host(db, "coder", "existing.com");
    return db;
}

static void teardown(sqlite3 *db) {
    db_close(db);
    system("rm -rf " TEST_AGENTS_DIR);
}

/* Full approve flow: agent requests whitelist_host → pending → admin approves →
 * config updated → inbox receives confirmation */
static void test_approve_flow(void) {
    sqlite3 *db = setup();

    /* Create session in running state (agent is executing) */
    int64_t sid = session_create(db, NULL, "coder", -1, 0);
    assert(sid > 0);
    assert(session_set_state(db, sid, "running") == 0);

    /* Agent calls approval_request tool */
    ToolRegistry reg;
    tools_init(&reg);
    ToolApprovalCtx ctx = {.db = db, .session_id = sid, .agent_name = "coder"};
    tool_approval_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "approval_request");
    assert(e);
    char *result = e->handler(
        "{\"type\":\"whitelist_host\",\"payload\":{\"host\":\"api.newsite.com\"}}",
        e->user_data);
    assert(result);
    assert(strstr(result, "pending") != NULL);
    free(result);

    /* Verify approval is pending in DB */
    int count = 0;
    Approval *list = approval_list_pending(db, &count);
    assert(count == 1);
    int64_t aid = list[0].id;
    assert(strcmp(list[0].type, "whitelist_host") == 0);
    assert(strstr(list[0].payload, "api.newsite.com") != NULL);
    approval_list_free(list, count);

    /* Simulate: agent exits into waiting state */
    assert(session_set_state(db, sid, "waiting") == 0);

    /* Admin approves (simulating handle_approval_callback logic) */
    int rc = approval_resolve(db, aid, "approved", 12345);
    assert(rc == 0);

    /* Apply the approval — whitelist_host adds to agent config in DB */
    Approval *a = approval_get(db, aid);
    assert(a);
    rc = agent_config_add_host(db, a->agent_name, "api.newsite.com");
    assert(rc == 0);

    /* Post confirmation to inbox */
    inbox_insert(db, sid, "approval", "Approval approved: whitelist_host api.newsite.com");

    /* Transition waiting→idle */
    const char *sql = "UPDATE sessions SET state='idle' WHERE id=? AND state='waiting';";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    assert(sqlite3_changes(db) == 1);
    sqlite3_finalize(stmt);

    /* Verify config was updated in DB */
    size_t host_count = 0;
    char **hosts = agent_config_get_hosts(db, "coder", &host_count);
    assert(host_count == 2);
    int found = 0;
    for (size_t i = 0; i < host_count; i++) {
        if (strcmp(hosts[i], "api.newsite.com") == 0) found = 1;
        free(hosts[i]);
    }
    free(hosts);
    assert(found);

    /* Verify inbox has confirmation */
    int inbox_count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &inbox_count);
    assert(inbox_count == 1);
    assert(strstr(items[0].payload, "approved") != NULL);
    assert(strcmp(items[0].source, "approval") == 0);
    inbox_items_free(items, inbox_count);

    tools_free(&reg);
    teardown(db);
    printf("  PASS test_approve_flow\n");
}

/* Deny flow: agent requests → admin denies → inbox receives denial */
static void test_deny_flow(void) {
    sqlite3 *db = setup();

    int64_t sid = session_create(db, NULL, "coder", -1, 0);
    assert(session_set_state(db, sid, "running") == 0);

    /* Agent requests */
    ToolRegistry reg;
    tools_init(&reg);
    ToolApprovalCtx ctx = {.db = db, .session_id = sid, .agent_name = "coder"};
    tool_approval_register(&reg, &ctx);

    ToolEntry *e = tools_lookup(&reg, "approval_request");
    char *result = e->handler(
        "{\"type\":\"whitelist_host\",\"payload\":{\"host\":\"evil.com\"}}",
        e->user_data);
    assert(strstr(result, "pending") != NULL);
    free(result);

    int count = 0;
    Approval *list = approval_list_pending(db, &count);
    assert(count == 1);
    int64_t aid = list[0].id;
    approval_list_free(list, count);

    /* Agent waits */
    assert(session_set_state(db, sid, "waiting") == 0);

    /* Admin denies */
    int rc = approval_resolve(db, aid, "denied", 99);
    assert(rc == 0);

    /* Post denial to inbox */
    inbox_insert(db, sid, "approval", "Approval denied: whitelist_host");

    /* Transition waiting→idle */
    const char *sql = "UPDATE sessions SET state='idle' WHERE id=? AND state='waiting';";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    /* Verify config NOT updated — evil.com not in allowed_hosts */
    size_t host_count = 0;
    char **hosts = agent_config_get_hosts(db, "coder", &host_count);
    assert(host_count == 1);
    assert(strcmp(hosts[0], "existing.com") == 0);
    free(hosts[0]);
    free(hosts);

    /* Verify inbox has denial */
    int inbox_count = 0;
    InboxItem *items = inbox_peek(db, sid, 10, &inbox_count);
    assert(inbox_count == 1);
    assert(strstr(items[0].payload, "denied") != NULL);
    inbox_items_free(items, inbox_count);

    tools_free(&reg);
    teardown(db);
    printf("  PASS test_deny_flow\n");
}

/* V53: unauthorized approval attempt — non-admin can't resolve */
static void test_unauthorized_attempt(void) {
    int64_t admin_ids[] = {111, 222};
    Config cfg = {0};
    cfg.admin_chat_ids = admin_ids;
    cfg.admin_chat_id_count = 2;

    /* Admin passes */
    assert(telegram_is_admin(&cfg, 111) == 1);
    assert(telegram_is_admin(&cfg, 222) == 1);

    /* Non-admin rejected */
    assert(telegram_is_admin(&cfg, 999) == 0);
    assert(telegram_is_admin(&cfg, 0) == 0);

    /* In real flow, non-admin callback is silently ignored (V53).
     * Verify that resolve still requires the caller to check admin status —
     * approval_resolve itself doesn't enforce auth (that's the telegram layer's job).
     * But a resolved approval can't be re-resolved. */
    sqlite3 *db = db_open(":memory:");
    int64_t aid = approval_insert(db, 1, "coder", "whitelist_host", "{\"host\":\"x.com\"}");
    assert(aid > 0);

    /* First resolve succeeds (admin) */
    assert(approval_resolve(db, aid, "approved", 111) == 0);

    /* Second resolve fails (already resolved — even if admin tries again) */
    assert(approval_resolve(db, aid, "denied", 222) == -1);

    db_close(db);
    printf("  PASS test_unauthorized_attempt\n");
}

int main(void) {
    printf("test_approval_flow:\n");
    test_approve_flow();
    test_deny_flow();
    test_unauthorized_attempt();
    printf("All approval flow tests passed.\n");
    return 0;
}
