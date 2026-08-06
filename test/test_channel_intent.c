/* Surface interpretation (channel_intent.c concern 1) in isolation: chat
 * text → ChannelIntent and approval_decision payload → ChannelIntent.
 * Recognition must be exact — the whole point of the parser being pure is
 * that these edges are testable without a schema, a session, or a channel.
 * Authority and execution are covered end-to-end by test_channel_events. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "channel_intent.h"
#include "approval.h"

/* channel_intent.o references resolve_approval (execution concern, unused
 * here). The spy pre-satisfies the reference so resolve.o is never pulled —
 * same pattern as test_channel_events. */
void resolve_approval(int64_t approval_id, ApprovalDecision decision,
                      const char *decided_via, int64_t grant_expires_at) {
    (void)approval_id; (void)decision; (void)decided_via; (void)grant_expires_at;
    assert(!"resolve_approval must not be reached from the parser");
}

static void test_parse_decide(void) {
    ChannelIntent it = channel_intent_from_text("/approve 12");
    assert(it.kind == CHANNEL_INTENT_DECIDE);
    assert(it.origin == CHANNEL_ORIGIN_CHAT);
    assert(it.decide == CHANNEL_DECIDE_APPROVE);
    assert(it.approval_id == 12);

    it = channel_intent_from_text("/deny 7");
    assert(it.kind == CHANNEL_INTENT_DECIDE);
    assert(it.decide == CHANNEL_DECIDE_DENY);
    assert(it.approval_id == 7);

    /* Extra spaces are stripped; trailing junk after a space is tolerated. */
    it = channel_intent_from_text("/approve   12");
    assert(it.kind == CHANNEL_INTENT_DECIDE && it.approval_id == 12);
    it = channel_intent_from_text("/approve 12 please");
    assert(it.kind == CHANNEL_INTENT_DECIDE && it.approval_id == 12);

    /* Unusable arguments keep the DECIDE kind (the executor answers with
     * usage) but carry no id. */
    it = channel_intent_from_text("/approve");
    assert(it.kind == CHANNEL_INTENT_DECIDE && it.approval_id == 0);
    it = channel_intent_from_text("/deny banana");
    assert(it.kind == CHANNEL_INTENT_DECIDE && it.approval_id == 0);
    it = channel_intent_from_text("/approve 12x");
    assert(it.kind == CHANNEL_INTENT_DECIDE && it.approval_id == 0);
    it = channel_intent_from_text("/approve -3");
    assert(it.kind == CHANNEL_INTENT_DECIDE && it.approval_id == 0);

    /* Exactness: "/approvals" is the extension's listing command, not ours. */
    it = channel_intent_from_text("/approvals");
    assert(it.kind == CHANNEL_INTENT_NONE);
    printf("PASS\n");
}

static void test_parse_sessions(void) {
    ChannelIntent it = channel_intent_from_text("/new");
    assert(it.kind == CHANNEL_INTENT_NEW_SESSION);

    /* /new is exact — anything trailing makes it ordinary chat. */
    it = channel_intent_from_text("/new please");
    assert(it.kind == CHANNEL_INTENT_NONE);

    it = channel_intent_from_text("/sessions");
    assert(it.kind == CHANNEL_INTENT_LIST_SESSIONS);
    it = channel_intent_from_text("/sessions 5");
    assert(it.kind == CHANNEL_INTENT_PIN_SESSION && it.session_id == 5);
    /* A non-numeric pick falls back to the listing, mirroring strtoll. */
    it = channel_intent_from_text("/sessions abc");
    assert(it.kind == CHANNEL_INTENT_LIST_SESSIONS);

    it = channel_intent_from_text("/status");
    assert(it.kind == CHANNEL_INTENT_STATUS);
    it = channel_intent_from_text("/status of things");
    assert(it.kind == CHANNEL_INTENT_STATUS);
    printf("PASS\n");
}

static void test_parse_none(void) {
    assert(channel_intent_from_text("hello").kind == CHANNEL_INTENT_NONE);
    assert(channel_intent_from_text("").kind == CHANNEL_INTENT_NONE);
    assert(channel_intent_from_text(NULL).kind == CHANNEL_INTENT_NONE);
    assert(channel_intent_from_text("approve 12").kind == CHANNEL_INTENT_NONE);
    assert(channel_intent_from_text("/approved 12").kind == CHANNEL_INTENT_NONE);
    printf("PASS\n");
}

static void test_parse_decision_event(void) {
    sqlite3 *db;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    ChannelIntent it;

    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"yes\"}", &it) == 0);
    assert(it.kind == CHANNEL_INTENT_DECIDE);
    assert(it.origin == CHANNEL_ORIGIN_EVENT);
    assert(it.approval_id == 5);
    assert(it.decide == CHANNEL_DECIDE_APPROVE_ALWAYS);

    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"once\"}", &it) == 0);
    assert(it.decide == CHANNEL_DECIDE_APPROVE_ONCE);

    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"no\"}", &it) == 0);
    assert(it.decide == CHANNEL_DECIDE_DENY);

    /* Anything unrecognized is a deny, never an approve. */
    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"banana\"}", &it) == 0);
    assert(it.decide == CHANNEL_DECIDE_DENY);

    /* Malformed: missing/zero id, missing decision, not JSON at all. */
    assert(channel_intent_from_decision_event(db,
        "{\"decision\":\"yes\"}", &it) == -1);
    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":0,\"decision\":\"yes\"}", &it) == -1);
    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5}", &it) == -1);
    assert(channel_intent_from_decision_event(db, "not json", &it) == -1);

    sqlite3_close(db);
    printf("PASS\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    test_parse_decide();
    test_parse_sessions();
    test_parse_none();
    test_parse_decision_event();
    printf("All channel_intent tests passed\n");
    return 0;
}
