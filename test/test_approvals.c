/* Test approvals module: create, pending, resolve, expire, state transitions */
#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "approval.h"
#include "config_registry.h"
#include "db.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_approvals.db"

static void clean_db(void) {
    test_db_clean(DB_PATH);
}

static sqlite3 *fresh_db(void) {
    clean_db();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db != NULL);
    return db;
}

static void test_create_and_pending(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);

    int64_t id = approval_create(db, sid, "call_1", "request_config",
                                 APPROVAL_PARK_REQUIRED,
                                 "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"api.example.com\"]}}}",
                                 "apply");
    assert(id > 0);

    Approval *a = approval_get_pending(db, sid);
    assert(a != NULL);
    assert(a->id == id);
    assert(a->session_id == sid);
    assert(strcmp(a->tool_call_id, "call_1") == 0);
    assert(strcmp(a->tool_name, "request_config") == 0);
    assert(strcmp(a->park_reason, APPROVAL_PARK_REQUIRED) == 0);
    assert(strcmp(a->state, "pending") == 0);
    approval_free(a);

    db_close(db);
    clean_db();
    printf("  PASS: test_create_and_pending\n");
}

static void test_approve(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_2", "request_config",
                                 APPROVAL_PARK_REQUIRED,
                                 "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"tools\":[\"shell_exec\"]}}}",
                                 "apply");
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
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_3", "request_config",
                                 APPROVAL_PARK_REQUIRED,
                                 "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"evil.com\"]}}}",
                                 "apply");
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
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id1 = approval_create(db, sid, "c1", "request_config",
                                  APPROVAL_PARK_REQUIRED,
                                  "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"tmp.com\"]}}}",
                                  "apply");
    int64_t id2 = approval_create(db, sid, "c2", "request_config",
                                  APPROVAL_PARK_REQUIRED,
                                  "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"perm.com\"]}}}",
                                  "apply");
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
    db_agent_upsert(db, "bot", NULL, NULL);
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
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_x", "request_config",
                                 APPROVAL_PARK_REQUIRED,
                                 "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"bad.com\"]}}}",
                                 "apply");
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
    db_agent_upsert(db, "bot", NULL, NULL);

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
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    /* Create a pending approval (expires_at is in the future by default) */
    int64_t id = approval_create(db, sid, "call_e", "request_config",
                                 APPROVAL_PARK_REQUIRED,
                                 "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"exp.com\"]}}}",
                                 "apply");
    assert(id > 0);

    /* Not expired yet */
    int count = 0;
    int64_t *ids = approval_list_expired(db, NULL, &count);
    assert(count == 0 && ids == NULL);

    /* Force it expired */
    char sql[128];
    snprintf(sql, sizeof(sql),
             "UPDATE approvals SET expires_at = unixepoch() - 10 WHERE id = %lld",
             (long long)id);
    sqlite3_exec(db, sql, NULL, NULL, NULL);

    ids = approval_list_expired(db, NULL, &count);
    assert(count == 1);
    assert(ids[0] == id);
    free(ids);

    /* A fresh (future) approval should NOT be returned */
    int64_t id2 = approval_create(db, sid, "call_f", "request_config",
                                  APPROVAL_PARK_REQUIRED,
                                  "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"fresh.com\"]}}}",
                                  "apply");
    assert(id2 > 0);
    ids = approval_list_expired(db, NULL, &count);
    assert(count == 1);
    assert(ids[0] == id);
    free(ids);

    db_close(db);
    clean_db();
    printf("  PASS: test_approval_list_expired\n");
}

static void test_tool_mode(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);

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
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    /* No approval yet for this call. */
    assert(approval_get_for_tool_call(db, sid, "call_z") == NULL);

    int64_t id = approval_create(db, sid, "call_z", "email_send", APPROVAL_PARK_REQUIRED,
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
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_c", "email_send", APPROVAL_PARK_REQUIRED,
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

static void test_pending_subtree(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "root", NULL, NULL);
    db_agent_upsert(db, "sub", NULL, NULL);
    db_agent_upsert(db, "other", NULL, NULL);

    int64_t root = session_create(db, "root", "root", -1, 0);
    int64_t child = session_create(db, "child", "sub", root, 1);
    int64_t grandchild = session_create(db, "grandchild", "sub", child, 2);
    int64_t unrelated = session_create(db, "unrelated", "other", -1, 0);
    assert(root > 0 && child > 0 && grandchild > 0 && unrelated > 0);

    /* Nothing pending anywhere yet. */
    assert(approval_get_pending_subtree(db, root) == NULL);

    /* An approval parked on an unrelated tree must not be found. */
    int64_t unrelated_id = approval_create(db, unrelated, "call_u", "shell_exec",
                                           APPROVAL_PARK_REQUIRED, "{}", "rerun");
    assert(unrelated_id > 0);
    assert(approval_get_pending_subtree(db, root) == NULL);

    /* A grandchild's park is found via the root — the whole-subtree fix. */
    int64_t gc_id = approval_create(db, grandchild, "call_g", "request_config",
                                    APPROVAL_PARK_REQUIRED,
                                    "{\"action\":\"request_changes\",\"changes\":{\"grants\":{\"hosts\":[\"api.example.com\"]}}}",
                                    "apply");
    assert(gc_id > 0);

    Approval *found = approval_get_pending_subtree(db, root);
    assert(found != NULL);
    assert(found->id == gc_id);
    assert(found->session_id == grandchild);
    approval_free(found);

    /* Oldest-first: a later park on the child itself must not shadow the
     * grandchild's earlier one. */
    int64_t child_id = approval_create(db, child, "call_c", "shell_exec",
                                       APPROVAL_PARK_REQUIRED, "{}", "rerun");
    assert(child_id > 0);
    found = approval_get_pending_subtree(db, root);
    assert(found != NULL);
    assert(found->id == gc_id);
    approval_free(found);

    db_close(db);
    clean_db();
    printf("  PASS: test_pending_subtree\n");
}

/* approval_get_pending drives the park prompt; approval_get_pending_subtree
 * drives the CLI's y/n reader. With more than one park outstanding they must
 * name the SAME row, or the user is shown one approval and decides another. */
static void test_pending_order_matches_subtree(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);
    assert(sid > 0);

    int64_t first = approval_create(db, sid, "call_first", "shell_exec",
                                    APPROVAL_PARK_REQUIRED, "{}", "rerun");
    int64_t second = approval_create(db, sid, "call_second", "shell_exec",
                                     APPROVAL_PARK_REQUIRED, "{}", "rerun");
    assert(first > 0 && second > 0);

    Approval *a = approval_get_pending(db, sid);
    Approval *b = approval_get_pending_subtree(db, sid);
    assert(a != NULL && b != NULL);
    assert(a->id == first);
    assert(a->id == b->id);
    approval_free(a);
    approval_free(b);

    db_close(db);
    clean_db();
    printf("  PASS: test_pending_order_matches_subtree\n");
}

/* ── dedupe + ticket transfer (capability matching) ───────────────── */

static const char *ARGS_ASK_A =
    "{\"code\":\"fetch('https://x/1', 'token')\"}";
/* Mutated code — must NOT dedupe: capability match is semantic JSON equality,
 * and a differing value is differing authority (the placeholder-set special
 * case died with the secret_bind park, D17). */
static const char *ARGS_ASK_B =
    "{\"code\":\"var r = go('token'); use(r)\"}";

static void test_pending_match_dedupe(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_d1", "js_eval", APPROVAL_PARK_REQUIRED,
                                 ARGS_ASK_A, "rerun");
    assert(id > 0);

    /* Byte-identical re-issue → match (dedupe hit). */
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "js_eval",
                                       ARGS_ASK_A) == id);
    /* Mutated args → no match. */
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "js_eval",
                                       ARGS_ASK_B) == 0);
    /* Different tool → no match. */
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "file_write",
                                       ARGS_ASK_A) == 0);
    /* 'sensitive' stays per-call: an exact re-issue may dedupe, a variant
     * may not. */
    int64_t s2 = approval_create(db, sid, "call_d2", "web_fetch", APPROVAL_PARK_SENSITIVE,
                                 "{\"url\":\"https://bank.example\"}", "rerun");
    assert(s2 > 0);
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_SENSITIVE, "web_fetch",
                                       "{\"url\":\"https://bank.example\"}") == s2);
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_SENSITIVE, "web_fetch",
                                       "{\"url\":\"https://bank.example/x\"}") == 0);

    db_close(db);
    clean_db();
    printf("  PASS: test_pending_match_dedupe\n");
}

static void test_take_ticket(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    int64_t id = approval_create(db, sid, "call_t1", "js_eval", APPROVAL_PARK_REQUIRED,
                                 ARGS_ASK_A, "rerun");
    assert(id > 0);

    /* Pending rows are not tickets. */
    assert(approval_take_ticket(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", ARGS_ASK_A) == 0);

    Approval *a = approval_resolve(db, id, 1, "test");
    assert(a != NULL);
    approval_free(a);

    /* Approved + matching → consumed once; the second taker gets nothing.
     * A mutated re-issue is not a match. */
    assert(approval_take_ticket(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", ARGS_ASK_B) == 0);
    assert(approval_take_ticket(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", ARGS_ASK_A) == id);
    assert(approval_take_ticket(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", ARGS_ASK_A) == 0);

    /* An approved row past its park expiry is stale, not a ticket. */
    int64_t id2 = approval_create(db, sid, "call_t2", "js_eval", APPROVAL_PARK_REQUIRED,
                                  ARGS_ASK_A, "rerun");
    a = approval_resolve(db, id2, 1, "test");
    assert(a != NULL);
    approval_free(a);
    char sql[128];
    snprintf(sql, sizeof(sql),
             "UPDATE approvals SET expires_at = unixepoch()-5 WHERE id=%lld;",
             (long long)id2);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    assert(approval_take_ticket(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", ARGS_ASK_A) == 0);

    db_close(db);
    clean_db();
    printf("  PASS: test_take_ticket\n");
}

static void test_pending_ids(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    char buf[64];
    assert(approval_pending_ids(db, sid, buf, sizeof(buf)) == 0);
    assert(buf[0] == '\0');

    int64_t a1 = approval_create(db, sid, "c1", "js_eval", APPROVAL_PARK_REQUIRED, "{}", "rerun");
    int64_t a2 = approval_create(db, sid, "c2", "js_eval", APPROVAL_PARK_REQUIRED, "{}", "rerun");
    assert(a1 > 0 && a2 > 0);
    assert(approval_pending_ids(db, sid, buf, sizeof(buf)) == 2);
    char want[64];
    snprintf(want, sizeof(want), "#%lld #%lld", (long long)a1, (long long)a2);
    assert(strcmp(buf, want) == 0);

    db_close(db);
    clean_db();
    printf("  PASS: test_pending_ids\n");
}

/* Block window: global knob honours an explicit 0, and a session pinned to a
 * route on an ambient channel always resolves to 0 (never freeze a room). */
static void test_block_window_ambient(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t plain = session_create(db, "test", "bot", -1, 0);
    int64_t amb = session_create(db, "test", "bot", -1, 0);
    assert(plain > 0 && amb > 0);

    /* Unset → registry default (60), not the old 600. */
    assert(approval_block_seconds(db) == 60);
    assert(approval_block_seconds_for_session(db, plain) == 60);

    /* Two routes; only the second chat id is listed as ambient. */
    char sql[512];
    snprintf(sql, sizeof(sql),
             "INSERT INTO channel_routes(channel_name,chat_id,session_id) "
             "VALUES('discord','111',%lld),('discord','222',%lld);",
             (long long)plain, (long long)amb);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    /* Seeded directly: `<ext>.<key>` rows are minted by extension_install, so
     * config_set no-ops on a DB with no channel extension installed. Leading
     * space + a decoy entry: membership is exact, not substring. */
    assert(sqlite3_exec(db,
        "INSERT INTO config(key,value,default_value) "
        "VALUES('discord.ambient_channels','999, 222','');",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(approval_block_seconds_for_session(db, amb) == 0);
    assert(approval_block_seconds_for_session(db, plain) == 60);

    /* Explicit 0 is a value, not "unset" — config_get_int can't tell them apart. */
    config_set(db, "approval_block_sec", "0");
    assert(approval_block_seconds(db) == 0);
    assert(approval_block_seconds_for_session(db, plain) == 0);

    config_set(db, "approval_block_sec", "30");
    assert(approval_block_seconds_for_session(db, plain) == 30);
    assert(approval_block_seconds_for_session(db, amb) == 0);

    /* Clamped to the expiry deadline, as before. */
    config_set(db, "approval_timeout_sec", "10");
    assert(approval_block_seconds(db) == 10);

    char note[256];
    approval_background_notice(42, note, sizeof(note));
    assert(strstr(note, "approval 42 requested") != NULL);
    assert(strstr(note, "notified") != NULL);

    db_close(db);
    clean_db();
    printf("  PASS: test_block_window_ambient\n");
}

/* B1: canonicalization. The same request re-serialized (key order, whitespace)
 * is one capability — matching it is what kills the double-prompt. Differing
 * values, and differing array order, are still different requests. */
static void test_capability_match_canonical(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);
    int64_t sid = session_create(db, "test", "bot", -1, 0);

    const char *doc = "{\"host\":\"api.example.com\",\"ports\":[80,443]}";
    int64_t id = approval_create(db, sid, "call_c1", "request_config",
                                 APPROVAL_PARK_REQUIRED, doc, "rerun");
    assert(id > 0);

    /* Key order differs → same capability. */
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "request_config",
               "{\"ports\":[80,443],\"host\":\"api.example.com\"}") == id);
    /* Whitespace/indentation differs → same capability. */
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "request_config",
               "{ \"host\" : \"api.example.com\" ,\n  \"ports\" : [ 80, 443 ] }") == id);
    /* A changed value parks a new approval. */
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "request_config",
               "{\"host\":\"evil.example.com\",\"ports\":[80,443]}") == 0);
    /* Array ORDER matters — fullkey carries the index. */
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "request_config",
               "{\"host\":\"api.example.com\",\"ports\":[443,80]}") == 0);
    /* An extra key is a different request. */
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "request_config",
               "{\"host\":\"api.example.com\",\"ports\":[80,443],\"tls\":false}") == 0);

    /* The stored row keeps exactly what the model sent — audit trail intact. */
    Approval *a = approval_get_pending(db, sid);
    assert(a != NULL && strcmp(a->args_json, doc) == 0);
    approval_free(a);

    /* Empty containers are represented: {"a":{}} must not match {"a":1},
     * nor {"a":[]}, nor a populated object at the same key. */
    int64_t e = approval_create(db, sid, "call_c2", "js_eval", APPROVAL_PARK_REQUIRED,
                                "{\"a\":{}}", "rerun");
    assert(e > 0);
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", "{\"a\": {}}") == e);
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", "{\"a\":1}") == 0);
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", "{\"a\":[]}") == 0);
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", "{\"a\":{\"b\":1}}") == 0);
    assert(approval_find_pending_match(db, sid, APPROVAL_PARK_REQUIRED, "js_eval", "{}") == 0);

    db_close(db);
    clean_db();
    printf("  PASS: test_capability_match_canonical\n");
}

/* expires_at of a grant row; 0 for permanent (NULL), -1 when absent. */
static int64_t grant_expiry(sqlite3 *db, const char *agent, const char *kind,
                            const char *value) {
    sqlite3_stmt *st;
    assert(sqlite3_prepare_v2(db,
        "SELECT ifnull(expires_at, 0) FROM grants"
        " WHERE agent_name=?1 AND kind=?2 AND value=?3", -1, &st, NULL) == SQLITE_OK);
    sqlite3_bind_text(st, 1, agent, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 2, kind, -1, SQLITE_STATIC);
    sqlite3_bind_text(st, 3, value, -1, SQLITE_STATIC);
    int64_t v = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    return v;
}

/* M6: re-approving a still-live temporary grant extends expires_at; it never
 * shortens one. */
static void test_grant_expiry_extends(void) {
    sqlite3 *db = fresh_db();
    db_agent_upsert(db, "bot", NULL, NULL);

    int64_t now = (int64_t)time(NULL);
    assert(agent_config_grant(db, "bot", "host", "api.example.com", now + 60) == 0);
    assert(grant_expiry(db, "bot", "host", "api.example.com") == now + 60);

    /* Re-approval with a later deadline extends the live grant. */
    assert(agent_config_grant(db, "bot", "host", "api.example.com", now + 3600) == 0);
    assert(grant_expiry(db, "bot", "host", "api.example.com") == now + 3600);

    /* A shorter re-grant leaves the longer life alone. */
    assert(agent_config_grant(db, "bot", "host", "api.example.com", now + 120) == 0);
    assert(grant_expiry(db, "bot", "host", "api.example.com") == now + 3600);

    /* Permanent (no expiry) is the ultimate extension... */
    assert(agent_config_grant(db, "bot", "host", "api.example.com", 0) == 0);
    assert(grant_expiry(db, "bot", "host", "api.example.com") == 0);
    /* ...and a later temporary re-grant must not claw it back. */
    assert(agent_config_grant(db, "bot", "host", "api.example.com", now + 60) == 0);
    assert(grant_expiry(db, "bot", "host", "api.example.com") == 0);

    db_close(db);
    clean_db();
    printf("  PASS: test_grant_expiry_extends\n");
}

int main(void) {
    TEST_INIT();
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
    test_pending_subtree();
    test_pending_order_matches_subtree();
    test_pending_match_dedupe();
    test_capability_match_canonical();
    test_grant_expiry_extends();
    test_take_ticket();
    test_pending_ids();
    test_block_window_ambient();
    printf("all approval tests passed\n");
    return 0;
}
