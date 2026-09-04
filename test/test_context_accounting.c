/* Context accounting: the compaction trigger must see the whole request —
 * head (system prompt + tools) and provider/sent measures — not just the
 * entries' own estimates; and rescaled estimates must err high (chars/3). */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "context.h"
#include "db.h"
#include "test_util.h"

static sqlite3 *open_seeded(void) {
    sqlite3 *db = test_db_open(":memory:");
    if (db) test_seed_agent(db, "default");
    return db;
}

static int64_t append(sqlite3 *db, int64_t sid, Role role, const char *content,
                      int usage_in, int64_t it) {
    Message m = { .role = role, .content = (char *)content, .usage_in = usage_in,
                  .stop_reason = role == ROLE_ASSISTANT ? STOP_REASON_STOP : STOP_REASON_NONE };
    return entry_append_with_iteration(db, sid, &m, it);
}

static void add_tool(sqlite3 *db, const char *name, int param_bytes) {
    char *params = malloc(param_bytes + 1);
    memset(params, 'p', param_bytes); params[param_bytes] = 0;
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO tools(name, description, parameters_json, enabled) VALUES(?,'d',?,1)",
        -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
    sqlite3_bind_text(s, 2, params, -1, SQLITE_TRANSIENT);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    free(params);
}

/* Small entries, huge head: the old trigger (entries only) never fired and
 * the provider 400'd with the head alone over budget. */
static void test_large_head_triggers(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.compaction = 1;
    cfg.context_window = 10000;      /* limit = 6000 tokens */
    int64_t it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    assert(session_needs_compaction(db, sid, &cfg) == 0);

    add_tool(db, "fat", 30000);      /* ~10000 head tokens */
    int head = context_head_estimate(db, "default", sid, &cfg, "");
    assert(head >= 10000);
    assert(session_needs_compaction(db, sid, &cfg) == 1);
    db_close(db);
    printf("  PASS test_large_head_triggers\n");
}

/* The provider's prompt_tokens is trusted when fresh — it lifts the size
 * above our under-estimate — and ignored once a compaction post-dates it,
 * or the freshly compacted session would re-fire forever. */
static void test_provider_floor_and_staleness(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.compaction = 1;
    cfg.context_window = 10000;
    int64_t it = db_next_iteration_id(db, sid);
    int64_t e1 = append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    it = db_next_iteration_id(db, sid);
    int64_t e3 = append(db, sid, ROLE_USER, "more", 0, it);
    append(db, sid, ROLE_ASSISTANT, "sure", 9000, it);   /* provider says 9000 */
    assert(session_needs_compaction(db, sid, &cfg) == 1);

    /* Compact away the middle: the 9000 figure predates the summary. */
    assert(entry_compact(db, sid, e1, e3, "summary") > 0);
    assert(session_needs_compaction(db, sid, &cfg) == 0);

    /* A new post-compaction response reinstates the provider term. */
    it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "again", 0, it);
    append(db, sid, ROLE_ASSISTANT, "ok", 9500, it);
    assert(session_needs_compaction(db, sid, &cfg) == 1);
    db_close(db);
    printf("  PASS test_provider_floor_and_staleness\n");
}

/* The sent payload (llm_responses.request_body) bounds the estimate too. */
static void test_sent_payload_floor(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    Config cfg = {0};
    cfg.compaction = 1;
    cfg.context_window = 10000;
    int64_t it = db_next_iteration_id(db, sid);
    append(db, sid, ROLE_USER, "hi", 0, it);
    append(db, sid, ROLE_ASSISTANT, "hello", 0, it);
    assert(session_needs_compaction(db, sid, &cfg) == 0);

    char *body = malloc(30010);
    strcpy(body, "{\"x\":\"");
    memset(body + 6, 'y', 30000); strcpy(body + 30006, "\"}");
    sqlite3_stmt *s;
    assert(sqlite3_prepare_v2(db,
        "INSERT INTO llm_responses(session_id, iteration_id, status, request_body)"
        " VALUES(?, ?, 'ok', jsonb(?))", -1, &s, NULL) == SQLITE_OK);
    sqlite3_bind_int64(s, 1, sid);
    sqlite3_bind_int64(s, 2, it);
    sqlite3_bind_text(s, 3, body, -1, SQLITE_STATIC);
    assert(sqlite3_step(s) == SQLITE_DONE);
    sqlite3_finalize(s);
    free(body);
    assert(session_needs_compaction(db, sid, &cfg) == 1);
    db_close(db);
    printf("  PASS test_sent_payload_floor\n");
}

/* Estimates use chars/3 + 4, and the stored content_bytes lets a schema
 * patch rescale historical rows to the same rule. */
static void test_estimate_divisor(void) {
    sqlite3 *db = open_seeded();
    int64_t sid = session_create(db, "s", "default", -1, 0);
    int64_t it = db_next_iteration_id(db, sid);
    int64_t id = append(db, sid, ROLE_USER, "0123456789ab", 0, it);   /* 12 bytes */
    assert(db_scalar_i64(db, "SELECT token_estimate FROM entries WHERE id=?", id, -1)
           == TOKEN_ESTIMATE(12));
    assert(db_scalar_i64(db, "SELECT content_bytes/3 + 4 FROM entries WHERE id=?", id, -1)
           == TOKEN_ESTIMATE(12));
    db_close(db);
    printf("  PASS test_estimate_divisor\n");
}

int main(void) {
    TEST_INIT();
    printf("test_context_accounting:\n");
    test_large_head_triggers();
    test_provider_floor_and_staleness();
    test_sent_payload_floor();
    test_estimate_divisor();
    printf("All context accounting tests passed.\n");
    return 0;
}
