/* Test channel launcher tracking + channel_events consumer.
 * Verifies: channel_consume_events() itself — binding resolution (incl.
 * wildcard fallback), session find-or-create, the approval_decision event
 * type resolving structurally (bypassing routing entirely), plain-message
 * inbox delivery (never text-interpreted as a decision), and unconditional
 * event-row deletion. */
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

void resolve_approval(int64_t approval_id, ApprovalDecision decision, const char *decided_via,
                      int64_t grant_expires_at) {
    (void)grant_expires_at;
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

/* New-model route add: create a session bound to the agent, pin the chat. */
static int test_binding_set(sqlite3 *db, const char *type, const char *id, const char *agent) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT INTO sessions(name, agent_name, channel_name, chat_id)"
        " VALUES('route:%s:%s','%s','%s','%s');"
        "INSERT INTO channel_routes(channel_name, chat_id, session_id)"
        " VALUES('%s','%s',last_insert_rowid())"
        " ON CONFLICT(channel_name, chat_id)"
        " DO UPDATE SET session_id=excluded.session_id;",
        type, id, agent, type, id, type, id);
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
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
            "SELECT id FROM sessions WHERE channel_name=? AND chat_id=? AND agent_name=?"
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

/* agent_name columns (channel_routes, sessions) are enforced FKs (v31):
 * seed every agent this file routes to before any child rows land.
 * "Assistant" is the gate's default agent for admin/allow_unknown senders. */
static sqlite3 *open_seeded(const char *path) {
    sqlite3 *db = test_db_open(path);
    if (db) {
        test_seed_agent(db, "testagent");
        test_seed_agent(db, "Assistant");
    }
    return db;
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

/* consumer helpers (session_create etc.) resolve paths relative to CWD */
static char s_old_cwd[512];
static void chdir_work(void) {
    getcwd(s_old_cwd, sizeof(s_old_cwd));
    chdir(WORK_DIR);
}
static void chdir_restore(void) {
    chdir(s_old_cwd);
}

/* direct call into channel_consume_events() — binding resolves,
 * session is created + stamped, event routes to inbox, event row is deleted. */
static void test_channel_events_routing(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "user1", "testagent");
    test_event_insert(db, "mychannel", "message", "{\"chat_id\":\"user1\",\"text\":\"hello\"}");

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
    test_event_insert(db, "mychannel", "message", "{\"chat_id\":\"user1\",\"text\":\"again\"}");
    channel_consume_events(db);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 1);
    assert(test_inbox_count(db, sid) == 2);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Open door: no route, but channels.default_agent is set — the chat is
 * accepted, a session is created for its actual chat_id and pinned. */
static void test_channel_events_wildcard_fallback(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    assert(sqlite3_exec(db,
        "INSERT INTO channels(name, extension_name, type, binary_path,"
        "                     status, default_agent)"
        " VALUES('mychannel','mychannel','test','/x','active','testagent');",
        NULL, NULL, NULL) == SQLITE_OK);
    test_event_insert(db, "mychannel", "message", "{\"chat_id\":\"userX\",\"text\":\"hi\"}");

    channel_consume_events(db);

    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    int64_t sid = test_session_find(db, "mychannel", "userX", "testagent");
    assert(sid > 0);
    assert(test_inbox_count(db, sid) == 1);
    /* pin written back — the exact-route invariant holds from first contact */
    assert(test_scalar_count(db,
        "SELECT COUNT(*) FROM channel_routes"
        " WHERE channel_name='mychannel' AND chat_id='userX';") == 1);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* No binding at all: the gate drops the message (unknown sender, no
 * admin_ids match, no channel default_agent) — membership is not authority.
 * The event row is still consumed. */
static void test_channel_events_no_binding(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_event_insert(db, "unbound", "message", "{\"chat_id\":\"user1\",\"text\":\"hi\"}");

    channel_consume_events(db);

    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 0);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM inbox;") == 0);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Seed a channels row + registry keys so channel_config_get resolves for the
 * gate tests: channel `name` runs extension `name` with the given admin_ids. */
static void test_gate_channel_seed(sqlite3 *db, const char *name, const char *admin_ids) {
    char sql[512];
    snprintf(sql, sizeof(sql),
        "INSERT OR REPLACE INTO channels(name, extension_name, type, binary_path, status)"
        " VALUES('%s','%s','test','/x','active');"
        "INSERT INTO config(key, value, default_value, description)"
        " VALUES('%s.admin_ids','%s','','admins')"
        " ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
        name, name, name, admin_ids);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
}

/* Routing gate: unknown sender dropped + exactly one admin notification
 * (deduped per process); admin sender accepted via the global default_agent;
 * channels.default_agent restores the open-door behavior. */
static void test_channel_events_gate(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_gate_channel_seed(db, "gated", "777");

    /* Unknown sender: dropped, one notification row to the admin chat. */
    test_event_insert(db, "gated", "message",
        "{\"chat_id\":\"555\",\"text\":\"let me in\","
        "\"sender_id\":\"555\",\"sender_name\":\"Eve\",\"chat_type\":\"dm\"}");
    channel_consume_events(db);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 0);
    assert(test_scalar_count(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='gated'"
        " AND json_extract(payload,'$.chat_id')='777';") == 1);
    char *note = NULL;
    {
        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db,
            "SELECT json_extract(payload,'$.text') FROM channel_outbox"
            " WHERE channel_name='gated' LIMIT 1;", -1, &s, NULL) == SQLITE_OK);
        assert(sqlite3_step(s) == SQLITE_ROW);
        note = strdup((const char *)sqlite3_column_text(s, 0));
        sqlite3_finalize(s);
    }
    assert(note && strstr(note, "Eve") && strstr(note, "555")
           && strstr(note, "route add gated 555"));
    free(note);

    /* Same unknown sender again: still dropped, NO second notification. */
    test_event_insert(db, "gated", "message",
        "{\"chat_id\":\"555\",\"text\":\"hello?\",\"sender_id\":\"555\","
        "\"sender_name\":\"Eve\",\"chat_type\":\"dm\"}");
    channel_consume_events(db);
    assert(test_scalar_count(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='gated';") == 1);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 0);

    /* Admin sender (channel_id in admin_ids): accepted via default agent. */
    test_event_insert(db, "gated", "message",
        "{\"chat_id\":\"777\",\"text\":\"hi\",\"sender_id\":\"777\","
        "\"sender_name\":\"Op\",\"chat_type\":\"dm\"}");
    channel_consume_events(db);
    int64_t sid = test_session_find(db, "gated", "777", "Assistant");
    assert(sid > 0);
    assert(test_inbox_count(db, sid) == 1);

    /* Open door: channels.default_agent set — unrouted stranger accepted. */
    test_gate_channel_seed(db, "open", "");
    assert(sqlite3_exec(db,
        "UPDATE channels SET default_agent='Assistant' WHERE name='open';",
        NULL, NULL, NULL) == SQLITE_OK);
    test_event_insert(db, "open", "message",
        "{\"chat_id\":\"999\",\"text\":\"yo\",\"sender_id\":\"999\","
        "\"sender_name\":\"Sam\",\"chat_type\":\"dm\"}");
    channel_consume_events(db);
    assert(test_session_find(db, "open", "999", "Assistant") > 0);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Admin authority keys on sender_id, not chat_id. Discord's DM channel
 * snowflake is not the user's id, so a gate that matched admin_ids against
 * chat_id both denied the real admin and (inversely) handed authority to
 * whoever spoke in a chat that happens to be named like an admin id. */
static void test_channel_events_admin_is_sender(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_gate_channel_seed(db, "dsc", "u_admin");

    /* Admin user in a DM channel whose id is NOT the user id → accepted. */
    test_event_insert(db, "dsc", "message",
        "{\"chat_id\":\"dm_1\",\"text\":\"hi\",\"sender_id\":\"u_admin\","
        "\"sender_name\":\"Op\",\"chat_type\":\"dm\"}");
    channel_consume_events(db);
    int64_t sid = test_session_find(db, "dsc", "dm_1", "Assistant");
    assert(sid > 0);
    assert(test_inbox_count(db, sid) == 1);

    /* Inverse (positive control for the denial): a stranger speaking in a
     * chat whose id equals the admin's user id gets no authority. */
    test_event_insert(db, "dsc", "message",
        "{\"chat_id\":\"u_admin\",\"text\":\"let me in\",\"sender_id\":\"rando\","
        "\"sender_name\":\"Eve\",\"chat_type\":\"group\"}");
    channel_consume_events(db);
    assert(test_session_find(db, "dsc", "u_admin", "Assistant") < 0);

    /* /new is an authority action: the admin's rebinds the chat he spoke in. */
    test_event_insert(db, "dsc", "message",
        "{\"chat_id\":\"dm_1\",\"text\":\"/new\",\"sender_id\":\"u_admin\","
        "\"sender_name\":\"Op\",\"chat_type\":\"dm\"}");
    channel_consume_events(db);
    assert(test_scalar_count(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='dsc'"
        " AND json_extract(payload,'$.text') LIKE 'new session%';") == 1);

    /* …and the stranger's is not consumed as a command (no reply, no new
     * session — it falls through to the gate and is dropped). */
    test_event_insert(db, "dsc", "message",
        "{\"chat_id\":\"u_admin\",\"text\":\"/new\",\"sender_id\":\"rando\","
        "\"sender_name\":\"Eve\",\"chat_type\":\"group\"}");
    channel_consume_events(db);
    assert(test_scalar_count(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='dsc'"
        " AND json_extract(payload,'$.text') LIKE 'new session%';") == 1);
    assert(test_session_find(db, "dsc", "u_admin", "Assistant") < 0);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Route-to-session: the pin is the binding — a newer unpinned session for
 * the same chat is ignored until the pin is re-pointed; the FK refuses to
 * delete a session a route still pins. */
static void test_channel_events_session_pin(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "u7", "testagent");

    /* Two sessions for the chat; pin the older one on the route. */
    test_event_insert(db, "mychannel", "message", "{\"chat_id\":\"u7\",\"text\":\"one\"}");
    channel_consume_events(db);
    int64_t old_sid = test_session_find(db, "mychannel", "u7", "testagent");
    assert(old_sid > 0);
    assert(sqlite3_exec(db,
        "INSERT INTO sessions(name, agent_name, channel_name, chat_id)"
        " VALUES('newer','testagent','mychannel','u7');", NULL, NULL, NULL) == SQLITE_OK);
    int64_t new_sid = test_session_find(db, "mychannel", "u7", "testagent");
    assert(new_sid > old_sid);

    test_event_insert(db, "mychannel", "message", "{\"chat_id\":\"u7\",\"text\":\"two\"}");
    channel_consume_events(db);
    assert(test_inbox_count(db, old_sid) == 2);   /* pinned, not the newer one */
    assert(test_inbox_count(db, new_sid) == 0);

    /* Re-point the pin — subsequent messages land in the new session. */
    char sql[256];
    snprintf(sql, sizeof(sql),
        "UPDATE channel_routes SET session_id=%lld"
        " WHERE channel_name='mychannel' AND chat_id='u7';", (long long)new_sid);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    test_event_insert(db, "mychannel", "message", "{\"chat_id\":\"u7\",\"text\":\"three\"}");
    channel_consume_events(db);
    assert(test_inbox_count(db, new_sid) == 1);

    /* A pinned session cannot be deleted out from under its route (FK). */
    snprintf(sql, sizeof(sql), "DELETE FROM sessions WHERE id=%lld;", (long long)new_sid);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* F19: the entry content handed to the inbox is extracted plain text, never
 * the raw JSON envelope — bare text for DMs, sender-prefixed for groups.
 * Payloads without $.text (custom channels) pass through unchanged. */
static void test_channel_events_plain_text_content(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "dm1", "testagent");
    test_binding_set(db, "mychannel", "grp1", "testagent");
    test_binding_set(db, "mychannel", "raw1", "testagent");

    test_event_insert(db, "mychannel", "message",
        "{\"chat_id\":\"dm1\",\"text\":\"hello there\",\"sender_id\":\"1\","
        "\"sender_name\":\"Mark\",\"chat_type\":\"dm\"}");
    test_event_insert(db, "mychannel", "message",
        "{\"chat_id\":\"grp1\",\"text\":\"group hi\",\"sender_id\":\"1\","
        "\"sender_name\":\"Mark\",\"chat_type\":\"group\"}");
    test_event_insert(db, "mychannel", "message",
        "{\"chat_id\":\"raw1\",\"note\":\"custom shape\"}");
    channel_consume_events(db);

    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "SELECT i.payload FROM inbox i JOIN sessions se ON se.id=i.session_id"
        " WHERE se.chat_id=?1;", -1, &s, NULL) == SQLITE_OK);

    sqlite3_bind_text(s, 1, "dm1", -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "hello there") == 0);
    sqlite3_reset(s);

    sqlite3_bind_text(s, 1, "grp1", -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(s, 0), "Mark: group hi") == 0);
    sqlite3_reset(s);

    sqlite3_bind_text(s, 1, "raw1", -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_ROW);
    assert(strstr((const char *)sqlite3_column_text(s, 0), "custom shape"));
    sqlite3_finalize(s);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Non-"message" event types are deleted without routing, even with a
 * matching binding in place. */
static void test_channel_events_non_message_type(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "user1", "testagent");
    test_event_insert(db, "mychannel", "status", "{\"chat_id\":\"user1\",\"text\":\"typing\"}");

    channel_consume_events(db);

    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);
    /* binding_set pre-created the pinned session; nothing new, nothing routed */
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM sessions;") == 1);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM inbox;") == 0);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* approval_decision is a distinct, channel-agnostic event type carrying its
 * own approval_id — it resolves via resolve_approval() without touching
 * channel_routes or session lookup at all. Plain chat text is never
 * interpreted as a decision, even while an approval is pending: it always
 * lands in the inbox, and the approval stays pending. */
static void test_channel_events_approval_decision(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_binding_set(db, "mychannel", "user1", "testagent");

    int64_t sid = session_create(db, "mychannel", "testagent", -1, 0);
    assert(sid > 0);
    {
        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db,
            "UPDATE sessions SET channel_name=?, chat_id=? WHERE id=?;",
            -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_text(s, 1, "mychannel", -1, SQLITE_STATIC);
        sqlite3_bind_text(s, 2, "user1", -1, SQLITE_STATIC);
        sqlite3_bind_int64(s, 3, sid);
        assert(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }
    {
        /* re-point the chat's pin at this session — the pin is the binding */
        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db,
            "UPDATE channel_routes SET session_id=?"
            " WHERE channel_name='mychannel' AND chat_id='user1';",
            -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_int64(s, 1, sid);
        assert(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }

    /* decision "yes" -> APPROVAL_ALWAYS */
    {
        int64_t aid = approval_create(db, sid, "tc1", "shell_exec", "run", "{}", "rerun");
        assert(aid > 0);
        char payload[128];
        snprintf(payload, sizeof(payload),
                 "{\"approval_id\":%lld,\"decision\":\"yes\"}", (long long)aid);
        test_event_insert(db, "mychannel", "approval_decision", payload);
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

    /* decision "once" -> APPROVAL_ONCE */
    resolve_spy_reset();
    {
        int64_t aid = approval_create(db, sid, "tc2", "shell_exec", "run", "{}", "rerun");
        assert(aid > 0);
        char payload[128];
        snprintf(payload, sizeof(payload),
                 "{\"approval_id\":%lld,\"decision\":\"once\"}", (long long)aid);
        test_event_insert(db, "mychannel", "approval_decision", payload);
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

    /* decision "no" (and anything not "yes"/"once") -> APPROVAL_DENY */
    resolve_spy_reset();
    {
        int64_t aid = approval_create(db, sid, "tc3", "shell_exec", "run", "{}", "rerun");
        assert(aid > 0);
        char payload[128];
        snprintf(payload, sizeof(payload),
                 "{\"approval_id\":%lld,\"decision\":\"no\"}", (long long)aid);
        test_event_insert(db, "mychannel", "approval_decision", payload);
        channel_consume_events(db);

        assert(g_resolve_calls == 1);
        assert(g_resolve_approval_id == aid);
        assert(g_resolve_decision == APPROVAL_DENY);
        assert(strcmp(g_resolve_decided_via, "channel:mychannel") == 0);
        assert(test_inbox_count(db, sid) == 0);
        assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);

        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db, "UPDATE approvals SET state='denied' WHERE id=?;", -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_int64(s, 1, aid);
        assert(sqlite3_step(s) == SQLITE_DONE);
        sqlite3_finalize(s);
    }

    /* Ordinary chat text while an approval is pending is NOT an implicit
     * decision — it forwards to the inbox untouched, and the approval
     * stays pending for a later structural decision (or expiry). */
    resolve_spy_reset();
    {
        int64_t aid = approval_create(db, sid, "tc4", "shell_exec", "run", "{}", "rerun");
        assert(aid > 0);
        test_event_insert(db, "mychannel", "message", "{\"chat_id\":\"user1\",\"text\":\"yes\"}");
        channel_consume_events(db);

        assert(g_resolve_calls == 0);
        assert(test_inbox_count(db, sid) == 1);

        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db, "SELECT state FROM approvals WHERE id=?;", -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_int64(s, 1, aid);
        assert(sqlite3_step(s) == SQLITE_ROW);
        assert(strcmp((const char *)sqlite3_column_text(s, 0), "pending") == 0);
        sqlite3_finalize(s);
    }

    /* Cross-channel forgery (review-2 F1): approval bound to channel A, a
     * decision event arriving from channel B is ignored — resolve never
     * fires, the approval stays pending, the event is consumed. */
    resolve_spy_reset();
    {
        int64_t aid = approval_create(db, sid, "tc5", "shell_exec", "run", "{}", "rerun");
        assert(aid > 0);
        char payload[128];
        snprintf(payload, sizeof(payload),
                 "{\"approval_id\":%lld,\"decision\":\"yes\"}", (long long)aid);
        test_event_insert(db, "otherchannel", "approval_decision", payload);
        channel_consume_events(db);

        assert(g_resolve_calls == 0);
        assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_events;") == 0);

        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db, "SELECT state FROM approvals WHERE id=?;", -1, &s, NULL) == SQLITE_OK);
        sqlite3_bind_int64(s, 1, aid);
        assert(sqlite3_step(s) == SQLITE_ROW);
        assert(strcmp((const char *)sqlite3_column_text(s, 0), "pending") == 0);
        sqlite3_finalize(s);
    }

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* Verify default binding fallback */
static void test_channel_events_default_binding(void) {
    setup();

    sqlite3 *db = open_seeded(DB_PATH);
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

/* Verify channels table schema works */
static void test_channels_table(void) {
    setup();

    sqlite3 *db = open_seeded(DB_PATH);
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
    /* Lifecycle default is 'draft' — only --check/--activate reach 'active'. */
    assert(strcmp((const char *)sqlite3_column_text(stmt, 3), "draft") == 0);
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

/* /new and /sessions chat commands: admin-only, consumed in C before
 * dispatch; /new re-points the pin at a fresh session; /sessions lists;
 * /sessions <id> attaches. A non-admin's /new is an ordinary message. */
static void test_chat_commands(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    test_gate_channel_seed(db, "cmdch", "9");
    test_binding_set(db, "cmdch", "9", "testagent");
    int64_t first = test_session_find(db, "cmdch", "9", "testagent");
    assert(first > 0);

    /* /new: fresh session, pin re-pointed, reply queued, event consumed. */
    test_event_insert(db, "cmdch", "message", "{\"chat_id\":\"9\",\"text\":\"/new\"}");
    channel_consume_events(db);
    int64_t fresh = test_session_find(db, "cmdch", "9", "testagent");
    assert(fresh > first);
    assert(test_scalar_count(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='cmdch'"
        " AND json_extract(payload,'$.text') LIKE 'new session%';") == 1);
    assert(test_inbox_count(db, fresh) == 0);   /* command, not a message */

    /* next ordinary message lands in the fresh session */
    test_event_insert(db, "cmdch", "message", "{\"chat_id\":\"9\",\"text\":\"hi\"}");
    channel_consume_events(db);
    assert(test_inbox_count(db, fresh) == 1);
    assert(test_inbox_count(db, first) == 0);

    /* /sessions lists both, current pin starred */
    test_event_insert(db, "cmdch", "message", "{\"chat_id\":\"9\",\"text\":\"/sessions\"}");
    channel_consume_events(db);
    {
        sqlite3_stmt *s;
        assert(sqlite3_prepare_v2(db,
            "SELECT json_extract(payload,'$.text') FROM channel_outbox"
            " WHERE channel_name='cmdch' ORDER BY id DESC LIMIT 1;",
            -1, &s, NULL) == SQLITE_OK);
        assert(sqlite3_step(s) == SQLITE_ROW);
        const char *t = (const char *)sqlite3_column_text(s, 0);
        char star[32];
        snprintf(star, sizeof(star), "* #%lld", (long long)fresh);
        assert(t && strstr(t, star));
        sqlite3_finalize(s);
    }

    /* /sessions <first>: re-attach the old session */
    {
        char cmd[96];
        snprintf(cmd, sizeof(cmd),
                 "{\"chat_id\":\"9\",\"text\":\"/sessions %lld\"}", (long long)first);
        test_event_insert(db, "cmdch", "message", cmd);
    }
    channel_consume_events(db);
    test_event_insert(db, "cmdch", "message", "{\"chat_id\":\"9\",\"text\":\"back\"}");
    channel_consume_events(db);
    assert(test_inbox_count(db, first) == 1);

    /* non-admin /new: not a command — routed to the chat's session as text */
    test_binding_set(db, "cmdch", "55", "testagent");
    int64_t other = test_session_find(db, "cmdch", "55", "testagent");
    test_event_insert(db, "cmdch", "message", "{\"chat_id\":\"55\",\"text\":\"/new\"}");
    channel_consume_events(db);
    assert(test_inbox_count(db, other) == 1);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* channel_notify_session: outbox row lands for channel-bound sessions,
 * silently no-ops for channel-less ones (CLI, sub-agents). */
static void test_notify_session(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);

    int64_t bound = session_create(db, "s-bound", "testagent", -1, 0);
    sqlite3_exec(db, "UPDATE sessions SET channel_name='mychannel', chat_id='42'"
                     " WHERE name='s-bound';", NULL, NULL, NULL);
    int64_t bare = session_create(db, "s-bare", "testagent", -1, 0);

    assert(channel_notify_session(db, NULL, bound, "model degraded") == 0);
    assert(channel_notify_session(db, NULL, bare, "model degraded") == 0);
    assert(test_scalar_count(db, "SELECT COUNT(*) FROM channel_outbox;") == 1);
    assert(test_scalar_count(db,
        "SELECT COUNT(*) FROM channel_outbox WHERE channel_name='mychannel'"
        " AND json_extract(payload,'$.chat_id')='42'"
        " AND json_extract(payload,'$.text')='model degraded';") == 1);

    db_close(db);
    cleanup();
    printf("PASS\n");
}

/* ───── tool_filter tests ───── */

/* Helper: route add --tools equivalent — the filter freezes onto the
 * session created for the pin (route keeps a copy for display). */
static int test_binding_set_filtered(sqlite3 *db, const char *type, const char *id,
                                     const char *agent, const char *tool_filter) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO sessions(name, agent_name, channel_name, chat_id, tool_filter)"
            " VALUES('route:'||?1||':'||?2, ?3, ?1, ?2, ?4)",
            -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, type, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, id, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 3, agent, -1, SQLITE_STATIC);
    if (tool_filter)
        sqlite3_bind_text(s, 4, tool_filter, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 4);
    int rc = sqlite3_step(s); sqlite3_finalize(s);
    if (rc != SQLITE_DONE) return -1;
    int64_t sid = sqlite3_last_insert_rowid(db);
    if (sqlite3_prepare_v2(db,
            "INSERT INTO channel_routes(channel_name, chat_id, session_id, tool_filter)"
            " VALUES(?1,?2,?3,?4)"
            " ON CONFLICT(channel_name, chat_id) DO UPDATE SET"
            "  session_id=excluded.session_id, tool_filter=excluded.tool_filter",
            -1, &s, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(s, 1, type, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(s, 3, sid);
    if (tool_filter)
        sqlite3_bind_text(s, 4, tool_filter, -1, SQLITE_STATIC);
    else
        sqlite3_bind_null(s, 4);
    rc = sqlite3_step(s); sqlite3_finalize(s);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Helper: read sessions.tool_filter for a given session id.  Returns a
 * malloc'd string (caller frees) or NULL if the column is SQL NULL. */
static char *test_session_tool_filter(sqlite3 *db, int64_t sid) {
    sqlite3_stmt *s;
    if (sqlite3_prepare_v2(db,
            "SELECT tool_filter FROM sessions WHERE id=?;", -1, &s, NULL) != SQLITE_OK)
        return NULL;
    sqlite3_bind_int64(s, 1, sid);
    char *out = NULL;
    if (sqlite3_step(s) == SQLITE_ROW && sqlite3_column_type(s, 0) != SQLITE_NULL)
        out = strdup((const char *)sqlite3_column_text(s, 0));
    sqlite3_finalize(s);
    return out;
}

/* 1. Route with tool_filter → the pinned session carries that filter. */
static void test_tool_filter_exact_route(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    assert(test_binding_set_filtered(db, "fch", "u1", "testagent",
                                     "[\"file_read\",\"web_fetch\"]") == 0);
    test_event_insert(db, "fch", "message", "{\"chat_id\":\"u1\",\"text\":\"hi\"}");
    channel_consume_events(db);

    int64_t sid = test_session_find(db, "fch", "u1", "testagent");
    assert(sid > 0);
    char *f = test_session_tool_filter(db, sid);
    assert(f);
    assert(strcmp(f, "[\"file_read\",\"web_fetch\"]") == 0);
    free(f);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* 3. Gate-accepted chats (admin / open door, no pre-made route) → session
 *    tool_filter is NULL. */
static void test_tool_filter_admin_unrouted(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    /* Seed channel with admin_ids + open-door default_agent, no route row. */
    test_gate_channel_seed(db, "adm", "888");
    assert(sqlite3_exec(db,
        "UPDATE channels SET default_agent='Assistant' WHERE name='adm';",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Admin sender (in admin_ids, no route row). */
    test_event_insert(db, "adm", "message",
        "{\"chat_id\":\"888\",\"text\":\"hi\",\"sender_id\":\"888\","
        "\"sender_name\":\"Boss\",\"chat_type\":\"dm\"}");
    channel_consume_events(db);

    int64_t sid_admin = test_session_find(db, "adm", "888", "Assistant");
    assert(sid_admin > 0);
    char *f1 = test_session_tool_filter(db, sid_admin);
    assert(f1 == NULL);  /* no route row → unrestricted */

    /* open-door stranger (no pre-made route). */
    test_event_insert(db, "adm", "message",
        "{\"chat_id\":\"999\",\"text\":\"hey\",\"sender_id\":\"999\","
        "\"sender_name\":\"Rando\",\"chat_type\":\"dm\"}");
    channel_consume_events(db);

    int64_t sid_stranger = test_session_find(db, "adm", "999", "Assistant");
    assert(sid_stranger > 0);
    char *f2 = test_session_tool_filter(db, sid_stranger);
    assert(f2 == NULL);  /* no route row → unrestricted */

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

/* 4. Pre-existing session: changing route's tool_filter after session creation
 *    does NOT retro-apply to the existing session. */
static void test_tool_filter_no_retro_apply(void) {
    setup();
    sqlite3 *db = open_seeded(DB_PATH);
    assert(db);
    chdir_work();

    /* Route without filter → session created with NULL tool_filter. */
    assert(test_binding_set_filtered(db, "fch", "u5", "testagent", NULL) == 0);
    test_event_insert(db, "fch", "message", "{\"chat_id\":\"u5\",\"text\":\"first\"}");
    channel_consume_events(db);

    int64_t sid = test_session_find(db, "fch", "u5", "testagent");
    assert(sid > 0);
    char *f1 = test_session_tool_filter(db, sid);
    assert(f1 == NULL);  /* starts unrestricted */

    /* Now edit the filter on the route row only (a raw edit — route add
     * would mint a new session; the point is no retro-apply to the pin). */
    assert(sqlite3_exec(db,
        "UPDATE channel_routes SET tool_filter='[\"file_read\"]'"
        " WHERE channel_name='fch' AND chat_id='u5';",
        NULL, NULL, NULL) == SQLITE_OK);

    /* Send another event — reuses the existing session. */
    test_event_insert(db, "fch", "message", "{\"chat_id\":\"u5\",\"text\":\"second\"}");
    channel_consume_events(db);

    /* Still one session, and its filter is still NULL (not retro-applied). */
    assert(test_scalar_count(db,
        "SELECT COUNT(*) FROM sessions WHERE channel_name='fch' AND chat_id='u5';") == 1);
    char *f2 = test_session_tool_filter(db, sid);
    assert(f2 == NULL);  /* unchanged */

    /* But the inbox did get the second message (session is alive). */
    assert(test_inbox_count(db, sid) == 2);

    chdir_restore();
    db_close(db);
    printf("PASS\n");
}

int main(void) {
    TEST_INIT();
    alarm(10);

    printf("test_channel_events:\n");

    printf("  channel_events_routing... ");
    test_channel_events_routing();

    printf("  channel_events_wildcard_fallback... ");
    test_channel_events_wildcard_fallback();

    printf("  channel_events_no_binding... ");
    test_channel_events_no_binding();

    printf("  channel_events_gate... ");
    test_channel_events_gate();

    printf("  channel_events_admin_is_sender... ");
    test_channel_events_admin_is_sender();

    printf("  channel_events_session_pin... ");
    test_channel_events_session_pin();

    printf("  channel_events_plain_text_content... ");
    test_channel_events_plain_text_content();

    printf("  channel_events_non_message_type... ");
    test_channel_events_non_message_type();

    printf("  channel_events_approval_decision... ");
    test_channel_events_approval_decision();

    printf("  channel_events_default_binding... ");
    test_channel_events_default_binding();

    printf("  channels_table... ");
    test_channels_table();

    printf("  chat_commands... ");
    test_chat_commands();

    printf("  notify_session... ");
    test_notify_session();

    printf("  tool_filter_exact_route... ");
    test_tool_filter_exact_route();


    printf("  tool_filter_admin_unrouted... ");
    test_tool_filter_admin_unrouted();

    printf("  tool_filter_no_retro_apply... ");
    test_tool_filter_no_retro_apply();

    cleanup();
    printf("all channel_events tests passed\n");
    return 0;
}
