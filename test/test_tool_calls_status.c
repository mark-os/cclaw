/* tool_calls workflow state: pending → status, and the dispatch claim CAS. */
#include "db.h"
#include "test_util.h"
#include "db_response.h"
#include <assert.h>

static int64_t add_call(sqlite3 *db, int64_t sid, const char *call_id,
                        const char *name, const char *args) {
    int64_t eid = entry_append_typed(db, sid, 1, "tool_call", 0, args, call_id,
                                     name, 0, STOP_REASON_NONE, NULL, 0, 0, 0);
    assert(eid > 0);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(db, "INSERT INTO tool_calls(session_id, entry_id, call_id, name)"
                           " VALUES(?, ?, ?, ?);", -1, &stmt, NULL);
    sqlite3_bind_int64(stmt, 1, sid);
    sqlite3_bind_int64(stmt, 2, eid);
    sqlite3_bind_text(stmt, 3, call_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, name, -1, SQLITE_STATIC);
    assert(sqlite3_step(stmt) == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return eid;
}

static void test_pending_and_status(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    assert(sid > 0);
    add_call(db, sid, "call_1", "file_read", "{\"path\":\"x.txt\"}");

    /* Arguments come from entries.content (tool_calls has no args column). */
    int count = 0;
    PendingToolCall *list = db_tool_call_get_pending(db, sid, &count);
    assert(count == 1);
    assert(strcmp(list[0].call_id, "call_1") == 0);
    assert(strstr(list[0].arguments, "x.txt") != NULL);
    db_tool_call_free_pending(list, count);

    assert(db_tool_call_set_status(db, sid, "call_1", "awaiting_approval", NULL) == 0);
    list = db_tool_call_get_pending(db, sid, &count);
    assert(count == 0 && list == NULL);

    assert(db_tool_call_set_status(db, sid, "call_1", "done", "admin") == 0);
    char *by = test_qs(db, "SELECT resolved_by FROM tool_calls WHERE call_id='call_1'");
    assert(by && strcmp(by, "admin") == 0);
    free(by);
    db_close(db);
}

static void test_claim_cas(void) {
    /* r5 F10: the dispatch claim is a CAS — a second dispatcher (co-pointed
     * CLI vs daemon) must lose the claim, not double-dispatch. */
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    add_call(db, sid, "call_3", "shell_exec", "{}");

    /* First claim wins, second loses. */
    assert(db_tool_call_claim(db, sid, "call_3") == 1);
    assert(db_tool_call_claim(db, sid, "call_3") == 0);

    /* Claimed call is no longer pending (the loser's get_pending misses it
     * too — but the claim is what closes the read-then-dispatch race). */
    int count = 0;
    PendingToolCall *list = db_tool_call_get_pending(db, sid, &count);
    assert(count == 0 && list == NULL);

    /* Unclaim (park path): back to pending, claimable again. */
    db_tool_call_unclaim(db, sid, "call_3");
    list = db_tool_call_get_pending(db, sid, &count);
    assert(count == 1);
    db_tool_call_free_pending(list, count);
    assert(db_tool_call_claim(db, sid, "call_3") == 1);

    /* Unclaim is a CAS too: a call that completed meanwhile stays done. */
    assert(db_tool_call_set_status(db, sid, "call_3", "done", NULL) == 0);
    db_tool_call_unclaim(db, sid, "call_3");
    char *st = test_qs(db, "SELECT status FROM tool_calls WHERE call_id='call_3'");
    assert(st && strcmp(st, "done") == 0);
    free(st);

    /* Claiming a done call fails — a stale dispatcher can't resurrect it. */
    assert(db_tool_call_claim(db, sid, "call_3") == 0);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    RUN(test_pending_and_status);
    RUN(test_claim_cas);
    return 0;
}
