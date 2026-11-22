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
    assert(it.answer == APPROVAL_ANSWER_YES);
    assert(it.approval_id == 12);

    it = channel_intent_from_text("/deny 7");
    assert(it.kind == CHANNEL_INTENT_DECIDE);
    assert(it.answer == APPROVAL_ANSWER_NO);
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

    it = channel_intent_from_text("/admin");
    assert(it.kind == CHANNEL_INTENT_ADMIN);
    it = channel_intent_from_text("/admin please");
    assert(it.kind == CHANNEL_INTENT_ADMIN);
    assert(channel_intent_from_text("/admins").kind == CHANNEL_INTENT_NONE);
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
    assert(it.answer == APPROVAL_ANSWER_YES);
    assert(it.sender[0] == '\0');   /* absent sender is not an error */

    /* The clicker rides the payload so decided_via can name them. */
    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"no\",\"sender\":\"Mark\"}", &it) == 0);
    assert(strcmp(it.sender, "Mark") == 0);

    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"once\"}", &it) == 0);
    assert(it.answer == APPROVAL_ANSWER_ONCE);

    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"no\"}", &it) == 0);
    assert(it.answer == APPROVAL_ANSWER_NO);

    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"always\"}", &it) == 0);
    assert(it.answer == APPROVAL_ANSWER_ALWAYS);

    /* A word that is not an answer decides nothing — neither an approval
     * nor a denial. */
    assert(channel_intent_from_decision_event(db,
        "{\"approval_id\":5,\"decision\":\"banana\"}", &it) == -1);

    /* What each answer means for each kind of request: the one rule. */
    ApprovalDecision d;
    const char *why;
    assert(approval_answer_fit(APPROVAL_SHAPE_CALL, APPROVAL_ANSWER_YES, &d, &why) == 0
           && d == APPROVAL_ONCE);
    assert(approval_answer_fit(APPROVAL_SHAPE_CALL, APPROVAL_ANSWER_ALWAYS, &d, &why) == 0
           && d == APPROVAL_ALWAYS);
    assert(approval_answer_fit(APPROVAL_SHAPE_SENSITIVE, APPROVAL_ANSWER_YES, &d, &why) == 0
           && d == APPROVAL_ONCE);
    assert(approval_answer_fit(APPROVAL_SHAPE_SENSITIVE, APPROVAL_ANSWER_ALWAYS, &d, &why) == -1
           && why);
    assert(approval_answer_fit(APPROVAL_SHAPE_APPLY, APPROVAL_ANSWER_YES, &d, &why) == 0
           && d == APPROVAL_ALWAYS);
    assert(approval_answer_fit(APPROVAL_SHAPE_APPLY, APPROVAL_ANSWER_ONCE, &d, &why) == -1
           && why);
    assert(approval_answer_fit(APPROVAL_SHAPE_APPLY, APPROVAL_ANSWER_NO, &d, &why) == 0
           && d == APPROVAL_DENY);
    assert(approval_answer_fit(APPROVAL_SHAPE_CALL, APPROVAL_ANSWER_NONE, &d, &why) == -1
           && !why);
    assert(approval_answer_from_word("Y") == APPROVAL_ANSWER_YES);
    assert(approval_answer_from_word("deny") == APPROVAL_ANSWER_NO);
    assert(approval_answer_from_word("yes please") == APPROVAL_ANSWER_NONE);
    assert(approval_answer_from_word(NULL) == APPROVAL_ANSWER_NONE);

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
