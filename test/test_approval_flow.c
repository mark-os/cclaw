/* T151: approval flow end-to-end
 * V54: agent requests → admin approves/denies → config updated → inbox confirmation
 * V53: unauthorized approval attempt rejected */
#define _POSIX_C_SOURCE 200809L
#include "db.h"
#include "daemon.h"
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
    assert(strncmp(result, "AGENT_EXIT_APPROVAL:", 20) == 0);
    free(result);

    /* T201: Simulate daemon's role — insert approval from tool_call args */
    int64_t aid = approval_insert(db, sid, "coder", NULL,
                                  "whitelist_host", "{\"host\":\"api.newsite.com\"}");
    assert(aid > 0);

    /* Verify approval is pending in DB */
    int count = 0;
    Approval *list = approval_list_pending(db, &count);
    assert(count == 1);
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
    assert(strncmp(result, "AGENT_EXIT_APPROVAL:", 20) == 0);
    free(result);

    /* T201: Simulate daemon's role — insert approval from tool_call args */
    int64_t aid = approval_insert(db, sid, "coder", NULL,
                                  "whitelist_host", "{\"host\":\"evil.com\"}");
    assert(aid > 0);

    int count = 0;
    Approval *list = approval_list_pending(db, &count);
    assert(count == 1);
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
    int64_t aid = approval_insert(db, 1, "coder", NULL, "whitelist_host", "{\"host\":\"x.com\"}");
    assert(aid > 0);

    /* First resolve succeeds (admin) */
    assert(approval_resolve(db, aid, "approved", 111) == 0);

    /* Second resolve fails (already resolved — even if admin tries again) */
    assert(approval_resolve(db, aid, "denied", 222) == -1);

    db_close(db);
    printf("  PASS test_unauthorized_attempt\n");
}

/* T203/V78: daemon_resolve_approval updates PENDING entry + transitions state */
static void test_resolve_approval_updates_pending(void) {
    /* daemon_resolve_approval uses relative path "agents/<name>/agent.db" */
    system("rm -rf /tmp/test_t203 && mkdir -p /tmp/test_t203/agents/coder");
    assert(chdir("/tmp/test_t203") == 0);

    sqlite3 *adb = db_open_agent("agents/coder/agent.db");
    assert(adb);

    /* Create session in waiting state */
    int64_t sid = session_create(adb, NULL, "coder", -1, 0);
    assert(sid > 0);
    assert(session_set_state(adb, sid, "running") == 0);
    assert(session_set_state(adb, sid, "waiting") == 0);

    /* Insert a PENDING tool_result entry (simulating what agent wrote before exit 3) */
    const char *ins = "INSERT INTO entries (session_id, role, content, tool_call_id, turn_id)"
                      " VALUES (?, 3, 'PENDING', 'tc_approval_123', 1);";
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(adb, ins, -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, sid);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);

    db_close(adb);

    /* Call daemon_resolve_approval — opens agent DB by constructed path */
    int rc = daemon_resolve_approval("coder", sid, "tc_approval_123",
                                     "Approval #1 approved: whitelist_host");
    assert(rc == 0);

    /* Verify: reopen DB and check entry was updated */
    adb = db_open_agent("agents/coder/agent.db");
    assert(adb);

    const char *qsql = "SELECT content FROM entries WHERE session_id=? AND tool_call_id='tc_approval_123';";
    sqlite3_stmt *qs;
    assert(sqlite3_prepare_v2(adb, qsql, -1, &qs, NULL) == SQLITE_OK);
    sqlite3_bind_int64(qs, 1, sid);
    assert(sqlite3_step(qs) == SQLITE_ROW);
    const char *content = (const char *)sqlite3_column_text(qs, 0);
    assert(content != NULL);
    assert(strstr(content, "approved") != NULL);
    assert(strcmp(content, "PENDING") != 0);
    sqlite3_finalize(qs);

    /* Verify session transitioned to idle */
    const char *ssql = "SELECT state FROM sessions WHERE id=?;";
    sqlite3_stmt *ss;
    assert(sqlite3_prepare_v2(adb, ssql, -1, &ss, NULL) == SQLITE_OK);
    sqlite3_bind_int64(ss, 1, sid);
    assert(sqlite3_step(ss) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(ss, 0), "idle") == 0);
    sqlite3_finalize(ss);

    db_close(adb);
    system("rm -rf /tmp/test_t203");
    printf("  PASS test_resolve_approval_updates_pending\n");
}

int main(void) {
    printf("test_approval_flow:\n");
    test_approve_flow();
    test_deny_flow();
    test_unauthorized_attempt();
    test_resolve_approval_updates_pending();
    printf("All approval flow tests passed.\n");
    return 0;
}
