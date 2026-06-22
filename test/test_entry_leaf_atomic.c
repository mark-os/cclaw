/* Group 1 (S1/S2): entry append + branch-leaf advance must be atomic.
 * The entries_leaf_ai trigger advances sessions.leaf_id as part of the same
 * INSERT, so the leaf always points at the last appended entry and no entry is
 * ever orphaned from the branch — even across many appends. Compaction entries
 * are excluded from the trigger and must NOT move the leaf. */
#include "db.h"
#include "test_util.h"
#include "types.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int64_t leaf_of(sqlite3 *db, int64_t sid) {
    return db_scalar_i64(db, "SELECT leaf_id FROM sessions WHERE id=?;", sid, -1);
}

static sqlite3 *setup(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    session_create(db, "test", NULL, -1, 0);
    return db;
}

/* The leaf tracks the last insert, and every entry's parent is the previous
 * leaf — a single unbroken chain, which is what the trigger guarantees. */
static void test_leaf_tracks_last_insert(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;
    int64_t prev = leaf_of(db, sid); /* initial leaf (-1 sentinel root) */

    for (int i = 0; i < 50; i++) {
        Message msg = { .role = ROLE_USER, .content = "hi" };
        int64_t id = entry_append_with_turn(db, sid, &msg, 0 /* auto-turn */);
        assert(id > 0);
        /* leaf advanced to exactly this entry */
        assert(leaf_of(db, sid) == id);
        /* this entry's parent is the prior leaf — no orphan, chain intact */
        int64_t parent = db_scalar_i64(db,
            "SELECT parent_id FROM entries WHERE id=?;", id, -2);
        assert(parent == prev);
        prev = id;
    }

    /* Auto-turn (turn_id=0) computes a strictly increasing turn per append. */
    int64_t maxturn = db_scalar_i64(db,
        "SELECT MAX(turn_id) FROM entries WHERE session_id=?;", sid, -1);
    assert(maxturn == 50);
    sqlite3_close(db);
    printf("  test_leaf_tracks_last_insert OK\n");
}

/* entry_append_typed shares the same trigger path. */
static void test_typed_advances_leaf(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;
    int64_t id = entry_append_typed(db, sid, 1, "assistant_message", 0,
        "answer", NULL, NULL, 0, STOP_REASON_STOP, "m", 1, 1, 0);
    assert(id > 0);
    assert(leaf_of(db, sid) == id);
    sqlite3_close(db);
    printf("  test_typed_advances_leaf OK\n");
}

/* Compaction entries are inserted mid-branch and must NOT move the leaf. */
static void test_compaction_does_not_move_leaf(void) {
    sqlite3 *db = setup();
    int64_t sid = 1;

    Message m1 = { .role = ROLE_USER, .content = "first" };
    int64_t a = entry_append_with_turn(db, sid, &m1, 0);
    Message m2 = { .role = ROLE_ASSISTANT, .content = "second" };
    int64_t b = entry_append_with_turn(db, sid, &m2, 0);
    assert(leaf_of(db, sid) == b);

    /* Insert a compaction summary between a and b: leaf must stay at b. */
    int64_t c = entry_compact(db, sid, a, b, "summary");
    assert(c > 0);
    assert(leaf_of(db, sid) == b); /* unchanged */
    sqlite3_close(db);
    printf("  test_compaction_does_not_move_leaf OK\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_entry_leaf_atomic\n");
    test_leaf_tracks_last_insert();
    test_typed_advances_leaf();
    test_compaction_does_not_move_leaf();
    printf("PASS\n");
    return 0;
}
