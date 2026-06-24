/* Test approvals module: create, pending, resolve, expire, state transitions */
#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "approval.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_approvals.db"

static void clean_db(void) {
    unlink(DB_PATH);
    unlink(DB_PATH "-wal");
    unlink(DB_PATH "-shm");
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    return db;
}

static void test_create_and_pending(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);

    int64_t id = approval_create(db, sid, "call_1", "request_config",
                                 "grant_host", "{\"host\":\"api.example.com\"}", "apply");
    assert(id > 0);

    Approval *a = approval_get_pending(db, sid);
    assert(a != NULL);
    assert(a->id == id);
    assert(a->session_id == sid);
    assert(strcmp(a->tool_call_id, "call_1") == 0);
    assert(strcmp(a->tool_name, "request_config") == 0);
    assert(strcmp(a->action, "grant_host") == 0);
    assert(strcmp(a->state, "pending") == 0);
    approval_free(a);

    db_close(db);
    clean_db();
    printf("  PASS: test_create_and_pending\n");
}

static void test_approve(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_2", "request_config",
                                 "grant_tool", "{\"tool\":\"shell_exec\"}", "apply");
    assert(id > 0);

    Approval *a = approval_resolve(db, id, 1, "cli:user");
    assert(a != NULL);
    assert(strcmp(a->state, "approved") == 0);
    assert(strcmp(a->decided_via, "cli:user") == 0);
    approval_free(a);

    /* No longer pending */
    a = approval_get_pending(db, sid);
    assert(a == NULL);

    db_close(db);
    clean_db();
    printf("  PASS: test_approve\n");
}

static void test_deny(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_3", "request_config",
                                 "grant_host", "{\"host\":\"evil.com\"}", "apply");
    assert(id > 0);

    Approval *a = approval_resolve(db, id, 0, "auto:no-approver");
    assert(a != NULL);
    assert(strcmp(a->state, "denied") == 0);
    assert(strcmp(a->decided_via, "auto:no-approver") == 0);
    approval_free(a);

    /* Not pending anymore */
    a = approval_get_pending(db, sid);
    assert(a == NULL);

    db_close(db);
    clean_db();
    printf("  PASS: test_deny\n");
}

static void test_approve_and_deny_states(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id1 = approval_create(db, sid, "c1", "request_config",
                                  "grant_host", "{\"host\":\"tmp.com\"}", "apply");
    int64_t id2 = approval_create(db, sid, "c2", "request_config",
                                  "grant_host", "{\"host\":\"perm.com\"}", "apply");
    assert(id1 > 0 && id2 > 0);

    /* Approve first, deny second */
    Approval *a = approval_resolve(db, id1, 1, "cli");
    assert(a && strcmp(a->state, "approved") == 0);
    approval_free(a);
    a = approval_resolve(db, id2, 0, "cli");
    assert(a && strcmp(a->state, "denied") == 0);
    approval_free(a);

    /* Verify states */
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "SELECT state FROM approvals WHERE id=?", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, id1);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "approved") == 0);
    sqlite3_finalize(stmt);

    sqlite3_prepare_v2(db, "SELECT state FROM approvals WHERE id=?", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, id2);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "denied") == 0);
    sqlite3_finalize(stmt);

    db_close(db);
    clean_db();
    printf("  PASS: test_approve_and_deny_states\n");
}

static void test_session_set_state_awaiting_approval(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    /* idle → llm_running (legal) */
    assert(session_set_state(db, sid, "llm_running") == 0);

    /* llm_running → awaiting_approval (legal) */
    assert(session_set_state(db, sid, "awaiting_approval") == 0);

    /* awaiting_approval → tool_running (legal: approval resolved) */
    assert(session_set_state(db, sid, "tool_running") == 0);

    /* tool_running → awaiting_approval (legal) */
    assert(session_set_state(db, sid, "awaiting_approval") == 0);

    /* awaiting_approval → idle (legal) */
    assert(session_set_state(db, sid, "idle") == 0);

    /* idle → awaiting_approval (ILLEGAL) */
    assert(session_set_state(db, sid, "awaiting_approval") != 0);

    /* Verify awaiting_approval → llm_running (legal via the busy-states clause) */
    assert(session_set_state(db, sid, "tool_running") == 0);
    assert(session_set_state(db, sid, "awaiting_approval") == 0);
    assert(session_set_state(db, sid, "llm_running") == 0);

    db_close(db);
    clean_db();
    printf("  PASS: test_session_set_state_awaiting_approval\n");
}

static void test_fail_closed_denied(void) {
    /* A denied approval leaves no pending row — fail-closed behavior */
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_x", "request_config",
                                 "grant_host", "{\"host\":\"bad.com\"}", "apply");
    assert(id > 0);

    Approval *a = approval_resolve(db, id, 0, "auto:no-approver");
    assert(a != NULL);
    assert(strcmp(a->state, "denied") == 0);
    approval_free(a);

    /* No pending approval — any code checking get_pending gets NULL (fail-closed) */
    a = approval_get_pending(db, sid);
    assert(a == NULL);

    /* Double-resolve fails gracefully */
    a = approval_resolve(db, id, 1, "late");
    assert(a == NULL);

    db_close(db);
    clean_db();
    printf("  PASS: test_fail_closed_denied\n");
}

static void test_grant_caps_load(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);

    /* Grant with no expiry — should load */
    agent_config_grant(db, "bot", "host", "tmp.io", 0);
    AgentCaps caps;
    agent_caps_load(db, "bot", &caps);
    assert(caps.host_count == 1);
    assert(strcmp(caps.hosts[0], "tmp.io") == 0);
    agent_caps_free(&caps);

    /* Set expires_at to the past — should NOT load */
    sqlite3_exec(db, "UPDATE grants SET expires_at = unixepoch() - 10 WHERE value='tmp.io'",
                 NULL, NULL, NULL);
    agent_caps_load(db, "bot", &caps);
    assert(caps.host_count == 0);
    agent_caps_free(&caps);

    db_close(db);
    clean_db();
    printf("  PASS: test_grant_caps_load\n");
}

static void test_approval_list_expired(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    /* Create a pending approval (expires_at is in the future by default) */
    int64_t id = approval_create(db, sid, "call_e", "request_config",
                                 "grant_host", "{\"host\":\"exp.com\"}", "apply");
    assert(id > 0);

    /* Not expired yet */
    int count = 0;
    int64_t *ids = approval_list_expired(db, &count);
    assert(count == 0 && ids == NULL);

    /* Force it expired */
    char sql[128];
    snprintf(sql, sizeof(sql),
             "UPDATE approvals SET expires_at = unixepoch() - 10 WHERE id = %lld",
             (long long)id);
    sqlite3_exec(db, sql, NULL, NULL, NULL);

    ids = approval_list_expired(db, &count);
    assert(count == 1);
    assert(ids[0] == id);
    free(ids);

    /* A fresh (future) approval should NOT be returned */
    int64_t id2 = approval_create(db, sid, "call_f", "request_config",
                                  "grant_host", "{\"host\":\"fresh.com\"}", "apply");
    assert(id2 > 0);
    ids = approval_list_expired(db, &count);
    assert(count == 1);
    assert(ids[0] == id);
    free(ids);

    db_close(db);
    clean_db();
    printf("  PASS: test_approval_list_expired\n");
}

static void test_tool_mode(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);

    /* Ungranted tool → silent (run-freely default). */
    assert(agent_tool_mode(db, "bot", "email_send") == TOOL_MODE_SILENT);

    /* Grant it: still silent until a mode is set. */
    agent_config_grant(db, "bot", "tool", "email_send", 0);
    assert(agent_tool_mode(db, "bot", "email_send") == TOOL_MODE_SILENT);

    /* Tighten → always / tool_decides. */
    assert(agent_config_set_tool_mode(db, "bot", "email_send", "always") == 0);
    assert(agent_tool_mode(db, "bot", "email_send") == TOOL_MODE_ALWAYS);
    assert(agent_config_set_tool_mode(db, "bot", "email_send", "tool_decides") == 0);
    assert(agent_tool_mode(db, "bot", "email_send") == TOOL_MODE_DECIDES);

    /* Relax back to silent. */
    assert(agent_config_set_tool_mode(db, "bot", "email_send", "silent") == 0);
    assert(agent_tool_mode(db, "bot", "email_send") == TOOL_MODE_SILENT);

    /* Invalid mode rejected. */
    assert(agent_config_set_tool_mode(db, "bot", "email_send", "bogus") == -1);

    /* Setting the mode of an ungranted tool upserts the grant: "stop asking"
     * must persist even when no standing grant row exists yet. */
    assert(agent_config_set_tool_mode(db, "bot", "not_granted", "always") == 0);
    assert(agent_tool_mode(db, "bot", "not_granted") == TOOL_MODE_ALWAYS);
    assert(grants_contains(db, "bot", "tool", "not_granted") == 1);

    db_close(db);
    clean_db();
    printf("  PASS: test_tool_mode\n");
}

static void test_get_for_tool_call(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    /* No approval yet for this call. */
    assert(approval_get_for_tool_call(db, sid, "call_z") == NULL);

    int64_t id = approval_create(db, sid, "call_z", "email_send", "email_send",
                                 "{\"to\":\"a@b.c\"}", "rerun");
    assert(id > 0);

    Approval *a = approval_get_for_tool_call(db, sid, "call_z");
    assert(a && a->id == id && strcmp(a->state, "pending") == 0);
    approval_free(a);

    /* Scoped to the session: another session reusing the same tool_call_id
     * must NOT see this approval. */
    int64_t other = session_create(db, "test2", "bot", -1, 0);
    assert(approval_get_for_tool_call(db, other, "call_z") == NULL);

    /* After resolve, the latest row reflects the new state. */
    Approval *r = approval_resolve(db, id, 1, "cli");
    assert(r); approval_free(r);
    a = approval_get_for_tool_call(db, sid, "call_z");
    assert(a && strcmp(a->state, "approved") == 0);
    approval_free(a);

    db_close(db);
    clean_db();
    printf("  PASS: test_get_for_tool_call\n");
}

static void test_consume(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_c", "email_send", "email_send",
                                 "{\"to\":\"a@b.c\"}", "rerun");
    assert(id > 0);
    /* Cannot consume while still pending. */
    assert(approval_consume(db, id) == -1);

    Approval *r = approval_resolve(db, id, 1, "cli");
    assert(r); approval_free(r);

    /* First consume transitions approved → consumed. */
    assert(approval_consume(db, id) == 0);
    Approval *a = approval_get_for_tool_call(db, sid, "call_c");
    assert(a && strcmp(a->state, "consumed") == 0);
    approval_free(a);

    /* Second consume is a no-op: the once-grant is spent, so a replayed
     * tool_call_id no longer green-lights at the gate. */
    assert(approval_consume(db, id) == -1);

    db_close(db);
    clean_db();
    printf("  PASS: test_consume\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_approvals:\n");
    test_create_and_pending();
    test_approve();
    test_deny();
    test_approve_and_deny_states();
    test_session_set_state_awaiting_approval();
    test_fail_closed_denied();
    test_grant_caps_load();
    test_approval_list_expired();
    test_tool_mode();
    test_get_for_tool_call();
    test_consume();
    printf("all approval tests passed\n");
    return 0;
}
