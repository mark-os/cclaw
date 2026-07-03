/* T244/T245: Test channel launcher tracking + channel_events consumer.
 * Verifies: channel_consume_events() itself — binding resolution (incl.
 * wildcard fallback), session find-or-create, approval-decision routing
 * vs. plain inbox delivery, and unconditional event-row deletion. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>
#include "channel_api.h"
#include "channel.h"
#include "approval.h"
#include "resolve.h"

#include "db.h"
#include "test_util.h"

/* channel.o references resolve_approval (normally defined in main.c, which
 * isn't linked into libcclaw.a). Provide a spy here so channel_consume_events()
 * links and we can assert on how approval decisions were routed. */
static int64_t g_resolve_approval_id;
static ApprovalDecision g_resolve_decision;
static char g_resolve_decided_via[128];
static int g_resolve_calls;

void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via) {
    g_resolve_calls++;
    g_resolve_approval_id = approval_id;
    g_resolve_decision = decision;
    snprintf(g_resolve_decided_via, sizeof(g_resolve_decided_via), "%s", decided_via ? decided_via : "");
}

static void resolve_spy_reset(void) {
    g_resolve_calls = 0;
    g_resolve_approval_id = -1;
    g_resolve_decision = APPROVAL_DENY;
    g_resolve_decided_via[0] = '\0';
}

static int test_binding_set(sqlite3 *db, const char *type, const char *id, const char *agent) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO channel_routes(channel_name,channel_id,agent_name) VALUES(?,?,?)", -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, type, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, agent, -1, SQLITE_STATIC);
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}
static int test_inbox_count(sqlite3 *db, int64_t session_id) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM inbox WHERE session_id=? AND consumed=0", -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_int64(s, 1, session_id);
    int c = 0; if (sqlite3_step(s) == SQLITE_ROW) c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s); return c;
}
static int test_scalar_count(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    int c = 0; if (sqlite3_step(s) == SQLITE_ROW) c = sqlite3_column_int(s, 0);
    sqlite3_finalize(s); return c;
}
static int64_t test_session_find(sqlite3 *db, const char *channel_name, const char *channel_id, const char *agent_name) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT id FROM sessions WHERE channel_name=? AND channel_id=? AND agent_name=?"
            " ORDER BY id DESC LIMIT 1;", -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, channel_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, agent_name, -1, SQLITE_STATIC);
    int64_t sid = -1;
    if (sqlite3_step(s) == SQLITE_ROW) sid = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return sid;
}
static void test_event_insert(sqlite3 *db, const char *channel_name, const char *event_type, const char *payload) {
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO channel_events(channel_name, event_type, payload) VALUES(?,?,?);",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, channel_name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, event_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, payload, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
}

static const char *DB_PATH = "/tmp/test_channel_events_daemon.db";
static const char *WORK_DIR = "/tmp/test_channel_events_work";

static void cleanup(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s %s %s-wal %s-shm %s.pipe",
             WORK_DIR, DB_PATH, DB_PATH, DB_PATH, DB_PATH);
    system(cmd);
}

static void setup(void) {
    cleanup();
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/agents/testagent/workspace", WORK_DIR);
    system(cmd);
    resolve_spy_reset();
}

/* T297: consumer helpers (session_create etc.) resolve paths relative to CWD */
static char s_old_cwd[512];
static void chdir_work(void) {
    getcwd(s_old_cwd, sizeof(s_old_cwd));
    chdir(WORK_DIR);
}
static void chdir_restore(void) {
    chdir(s_old_cwd);
}

/* T245: direct call into channel_consume_events() — binding resolves,
 * session is created + stamped, event routes to inbox, event row is deleted. */
static void test_channel_events_routing(void) {
    setup();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "user1", "testagent");
    test_event_insert(db, "mychannel", "message", "{\"channel_id\":\"user1\",\"text\":\"hello\"}");

    channel_consume_events(db);

    /* channel_events row consumed */
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);

    /* session created + stamped */
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 1);
    int64_t sid = test_session_find(db, "mychannel", "user1", "testagent");
    assert(sid > 0);

    /* inbox has the routed message */
    assert(test_inbox_count(db, sid) == 1);
    {
        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db,
            "SELECT payload FROM inbox WHERE session_id=? ORDER BY id DESC LIMIT 1;",
            -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_int64(s, 1, sid);
        assert(sqlite3_step(s) == SQLITE_ROW);
        const char *p = (const char *)sqlite3_column_text(s, 0);
        assert(p && strstr(p, "hello"));
        sqlite3_finalize(s);
    }

    /* second event for the same channel_id reuses the same session */
    test_event_insert(db, "mychannel", "message", "{\"channel_id\":\"user1\",\"text\":\"again\"}");
    channel_consume_events(db);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 1);
    assert(test_inbox_count(db, sid) == 2);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Wildcard binding fallback: no exact (channel, channel_id) route, but a
 * (channel, "*") route exists — the session still gets created for the
 * event's actual channel_id, not "*". */
static void test_channel_events_wildcard_fallback(void) {
    setup();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "*", "testagent");
    test_event_insert(db, "mychannel", "message", "{\"channel_id\":\"userX\",\"text\":\"hi\"}");

    channel_consume_events(db);

    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    int64_t sid = test_session_find(db, "mychannel", "userX", "testagent");
    assert(sid > 0);
    assert(test_inbox_count(db, sid) == 1);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* No binding at all for the channel: event is dropped (deleted, no
 * session/inbox side effects). */
static void test_channel_events_no_binding(void) {
    setup();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    chdir_work();

    test_event_insert(db, "unbound", "message", "{\"channel_id\":\"user1\",\"text\":\"hi\"}");

    channel_consume_events(db);

    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 0);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Non-"message" event types are deleted without routing, even with a
 * matching binding in place. */
static void test_channel_events_non_message_type(void) {
    setup();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "user1", "testagent");
    test_event_insert(db, "mychannel", "status", "{\"channel_id\":\"user1\",\"text\":\"typing\"}");

    channel_consume_events(db);

    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 0);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Session awaiting approval: a channel message routes to resolve_approval()
 * as a decision instead of landing in the inbox. */
static void test_channel_events_approval_routing(void) {
    setup();
    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "user1", "testagent");

    int64_t sid = session_create(db, "mychannel", "testagent", -1, 0);
    assert(sid > 0);
    {
        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db,
            "UPDATE sessions SET channel_name=?, channel_id=? WHERE id=?;",
            -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, "mychannel", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, "user1", -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 3, sid);
        assert(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }

    /* "y" -> APPROVAL_ALWAYS */
    {
        int64_t aid = approval_create(db, sid, "tc1", "shell_exec", "run", "{}", "rerun");
        assert(aid > 0);
        test_event_insert(db, "mychannel", "message", "{\"channel_id\":\"user1\",\"text\":\"y\"}");
        channel_consume_events(db);

        assert(g_resolve_calls == 1);
        assert(g_resolve_approval_id == aid);
        assert(g_resolve_decision == APPROVAL_ALWAYS);
        assert(strcmp(g_resolve_decided_via, "channel:mychannel") == 0);
        assert(test_inbox_count(db, sid) == 0);
        assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);

        /* spy doesn't actually transition approval state; move it out of the
         * way so the next case sees no pending approval. */
        {
            sqlite3_stmt *s;
            assert(sqlite3_prepare_v2(db, "UPDATE approvals SET state='approved' WHERE id=?;", -1, &s, NULL) == SQLITE_OK);
            sqlite3_bind_int64(s, 1, aid);
            assert(sqlite3_step(s) == SQLITE_DONE);
            sqlite3_finalize(s);
        }
    }

    /* "once" -> APPROVAL_ONCE */
    resolve_spy_reset();
    {
        int64_t aid = approval_create(db, sid, "tc2", "shell_exec", "run", "{}", "rerun");
        assert(aid > 0);
        test_event_insert(db, "mychannel", "message", "{\"channel_id\":\"user1\",\"text\":\"once\"}");
        channel_consume_events(db);

        assert(g_resolve_calls == 1);
        assert(g_resolve_approval_id == aid);
        assert(g_resolve_decision == APPROVAL_ONCE);
        assert(strcmp(g_resolve_decided_via, "channel:mychannel") == 0);
        assert(test_inbox_count(db, sid) == 0);

        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db, "UPDATE approvals SET state='approved' WHERE id=?;", -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_int64(s, 1, aid);
        assert(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }

    /* anything else -> APPROVAL_DENY */
    resolve_spy_reset();
    {
        int64_t aid = approval_create(db, sid, "tc3", "shell_exec", "run", "{}", "rerun");
        assert(aid > 0);
        test_event_insert(db, "mychannel", "message", "{\"channel_id\":\"user1\",\"text\":\"nope\"}");
        channel_consume_events(db);

        assert(g_resolve_calls == 1);
        assert(g_resolve_approval_id == aid);
        assert(g_resolve_decision == APPROVAL_DENY);
        assert(strcmp(g_resolve_decided_via, "channel:mychannel") == 0);
        assert(test_inbox_count(db, sid) == 0);
        assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    }

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* T245: Verify default binding fallback */
static void test_channel_events_default_binding(void) {
    setup();

    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    /* Set up "default" binding for channel type */
    test_binding_set(db, "webchat", "default", "testagent");

    /* Verify fallback: no specific binding for "user99", but "default" exists */
    char *agent = db_channel_binding_get(db, "webchat", "user99");
    /* Direct lookup fails */
    assert(agent == NULL);

    /* Fallback to "default" */
    agent = db_channel_binding_get(db, "webchat", "default");
    assert(agent);
    assert(strcmp(agent, "testagent") == 0);
    free(agent);

    db_close(db);
    printf("PASS\n");
}

/* T244: Verify channels table schema works */
static void test_channels_table(void) {
    setup();

    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);

    /* Insert a channel row */
    const char *sql = "INSERT INTO channels(name, type, binary_path)"
                      " VALUES('telegram','telegram','/usr/local/bin/channel_telegram');";
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);

    /* Read it back */
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db,
        "SELECT name, type, binary_path, status FROM channels WHERE name='telegram';",
        -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "telegram") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 1), "telegram") == 0);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 3), "active") == 0);
    sqlite3_finalize(stmt);

    /* Update pid (simulating channel_update_pid) */
    const char *usql = "UPDATE channels SET pid=1234 WHERE name='telegram';";
    assert(sqlite3_exec(db, usql, NULL, NULL, NULL) == SQLITE_OK);

    /* Update status (simulating channel_set_status) */
    const char *ssql = "UPDATE channels SET status='failed' WHERE name='telegram';";
    assert(sqlite3_exec(db, ssql, NULL, NULL, NULL) == SQLITE_OK);

    assert(sqlite3_prepare_v2(db,
        "SELECT status, pid FROM channels WHERE name='telegram';",
        -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), "failed") == 0);
    assert(sqlite3_column_int(stmt, 1) == 1234);
    sqlite3_finalize(stmt);

    db_close(db);
    printf("PASS\n");
}

int main(void) {
    alarm(10);

    printf("test_channel_events (T244/T245):\n");

    printf("  channel_events_routing... ");
    test_channel_events_routing();

    printf("  channel_events_wildcard_fallback... ");
    test_channel_events_wildcard_fallback();

    printf("  channel_events_no_binding... ");
    test_channel_events_no_binding();

    printf("  channel_events_non_message_type... ");
    test_channel_events_non_message_type();

    printf("  channel_events_approval_routing... ");
    test_channel_events_approval_routing();

    printf("  channel_events_default_binding... ");
    test_channel_events_default_binding();

    printf("  channels_table... ");
    test_channels_table();

    cleanup();
    printf("all channel_events tests passed\n");
    return 0;
}
