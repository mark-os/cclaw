#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"
#include "llm.h"
#include "test_util.h"
#include "types.h"

/* Every provider spelling maps to one stop reason; anything unknown is an error. */
static void test_map_stop_reason(void) {
    static const struct { const char *wire; StopReason want; } cases[] = {
        { "stop", STOP_REASON_STOP }, { "end", STOP_REASON_STOP },
        { "end_turn", STOP_REASON_STOP }, { NULL, STOP_REASON_STOP },
        { "length", STOP_REASON_LENGTH }, { "max_tokens", STOP_REASON_LENGTH },
        { "tool_calls", STOP_REASON_TOOL_USE }, { "function_call", STOP_REASON_TOOL_USE },
        { "tool_use", STOP_REASON_TOOL_USE },
        { "content_filter", STOP_REASON_ERROR }, { "network_error", STOP_REASON_ERROR },
        { "something_unknown", STOP_REASON_ERROR },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (map_stop_reason(cases[i].wire) != cases[i].want) {
            fprintf(stderr, "FAIL cases[%zu] %s\n", i, cases[i].wire ? cases[i].wire : "(null)");
            abort();
        }
    }
}

/* A user message stores no stop reason; every assistant reason reads back as written. */
static void test_stop_reasons_persist(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test_sr", NULL, -1, 0);
    assert(sid > 0);
    static const StopReason want[] = { STOP_REASON_NONE, STOP_REASON_STOP, STOP_REASON_LENGTH,
                                       STOP_REASON_TOOL_USE, STOP_REASON_ERROR,
                                       STOP_REASON_ABORTED };
    int n = (int)(sizeof(want) / sizeof(want[0]));
    for (int i = 0; i < n; i++) {
        Message msg = {.role = i ? ROLE_ASSISTANT : ROLE_USER, .content = "msg",
                       .stop_reason = want[i]};
        entry_append_with_iteration(db, sid, &msg, db_next_iteration_id(db, sid));
    }
    int count = 0;
    Entry *entries = session_get_branch(db, sid, &count);
    assert(entries && count == n);
    for (int i = 0; i < n; i++) {
        if (entries[i].message.stop_reason != want[i]) {
            fprintf(stderr, "FAIL entry %d: %d, want %d\n", i,
                    (int)entries[i].message.stop_reason, (int)want[i]);
            abort();
        }
    }
    entry_branch_free(entries, count);
    db_close(db);
}

/* A length-stopped answer must announce itself on the delivery path; every
 * other stop reason renders the model's bytes untouched. */
static void test_length_notice_rendered(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test_sr4", NULL, -1, 0);
    assert(sid > 0);

    Message cut = {.role = ROLE_ASSISTANT, .content = "here is the first half of the ans",
                   .stop_reason = STOP_REASON_LENGTH};
    entry_append_with_iteration(db, sid, &cut, db_next_iteration_id(db, sid));
    char *text = get_response_text(db, sid);
    assert(text && strstr(text, "here is the first half of the ans"));
    assert(strstr(text, RESPONSE_TRUNCATED_NOTICE));
    free(text);

    /* The notice is render-only: the stored entry keeps the model's bytes. */
    char *stored = db_scalar_text(db,
        "SELECT content FROM entries WHERE session_id=? ORDER BY id DESC LIMIT 1;", sid);
    assert(stored && strcmp(stored, "here is the first half of the ans") == 0);
    free(stored);

    /* A normal stop renders clean. */
    Message done = {.role = ROLE_ASSISTANT, .content = "all done", .stop_reason = STOP_REASON_STOP};
    entry_append_with_iteration(db, sid, &done, db_next_iteration_id(db, sid));
    text = get_response_text(db, sid);
    assert(text && strcmp(text, "all done") == 0);
    free(text);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    printf("--- test_stop_reason_persist ---\n");
    RUN(test_map_stop_reason);
    RUN(test_stop_reasons_persist);
    RUN(test_length_notice_rendered);
    return 0;
}
