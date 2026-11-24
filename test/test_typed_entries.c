#include "db.h"
#include "test_util.h"
#include "db_response.h"
#include "types.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A typed entry gets its role from its type, and a Message appended by role
 * gets its type from the role. */
static void test_type_and_role_derived(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    static const struct { const char *type; int part; const char *call_id, *tool; int role; } typed[] = {
        { "reasoning",         0, NULL,       NULL,        ROLE_ASSISTANT },
        { "assistant_message", 1, NULL,       NULL,        ROLE_ASSISTANT },
        { "tool_call",         2, "call_abc", "file_read", ROLE_ASSISTANT },
        { "tool_result",       0, "call_abc", "file_read", ROLE_TOOL },
    };
    for (size_t i = 0; i < sizeof(typed) / sizeof(typed[0]); i++) {
        int64_t id = entry_append_typed(db, sid, 1, typed[i].type, typed[i].part, "x",
                                        typed[i].call_id, typed[i].tool, 0, STOP_REASON_NONE,
                                        NULL, 0, 0, 0);
        char *sql = sqlite3_mprintf("SELECT COUNT(*) FROM entries WHERE id=%lld AND type=%Q"
                                    " AND part_index=%d AND role=%d AND tool_call_id IS %Q"
                                    " AND tool_name IS %Q", (long long)id, typed[i].type,
                                    typed[i].part, typed[i].role, typed[i].call_id, typed[i].tool);
        if (id <= 0 || test_qi(db, sql) != 1) {
            fprintf(stderr, "FAIL typed[%zu] %s\n", i, typed[i].type);
            abort();
        }
        sqlite3_free(sql);
    }
    static const struct { Role role; const char *type; } by_role[] = {
        { ROLE_USER, "user_message" }, { ROLE_ASSISTANT, "assistant_message" },
    };
    for (size_t i = 0; i < sizeof(by_role) / sizeof(by_role[0]); i++) {
        Message m = {.role = by_role[i].role, .content = "hello"};
        int64_t id = entry_append_with_iteration(db, sid, &m, 1);
        char *sql = sqlite3_mprintf("SELECT COUNT(*) FROM entries WHERE id=%lld AND type=%Q"
                                    " AND part_index=0", (long long)id, by_role[i].type);
        if (id <= 0 || test_qi(db, sql) != 1) {
            fprintf(stderr, "FAIL by_role[%zu] %s\n", i, by_role[i].type);
            abort();
        }
        sqlite3_free(sql);
    }
    db_close(db);
}

/* One response becomes reasoning, message and each call, in part order, and
 * every call opens a pending tool_calls row. */
static void test_ingest_typed(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);
    int64_t it = db_next_iteration_id(db, sid);
    const char *body =
        "{\"choices\":[{\"message\":{\"content\":\"Here are the results.\","
        "\"reasoning\":\"I need to think...\","
        "\"tool_calls\":[{\"id\":\"call_1\",\"function\":{\"name\":\"file_read\","
        "\"arguments\":\"{\\\"path\\\":\\\"/tmp\\\"}\"}},"
        "{\"id\":\"call_2\",\"function\":{\"name\":\"shell_exec\","
        "\"arguments\":\"{\\\"cmd\\\":\\\"ls\\\"}\"}}]},"
        "\"finish_reason\":\"tool_calls\"}],"
        "\"usage\":{\"prompt_tokens\":100,\"completion_tokens\":50}}";
    TypedIngestResult res;
    assert(db_ingest_response(db, sid, it, "gpt-4o", ENDPOINT_OPENAI, body, NULL, 1, &res)
           == LLM_RESP_OK);
    assert(res.assistant_entry_id > 0);

    char *parts = test_qs(db, "SELECT group_concat(type || part_index, ',') FROM"
                              " (SELECT type, part_index FROM entries ORDER BY part_index)");
    assert(parts && strcmp(parts, "reasoning0,assistant_message1,tool_call2,tool_call3") == 0);
    free(parts);
    char *calls = test_qs(db, "SELECT group_concat(call_id || ':' || name || ':' || status, ',')"
                              " FROM (SELECT * FROM tool_calls ORDER BY rowid)");
    assert(calls && strcmp(calls, "call_1:file_read:pending,call_2:shell_exec:pending") == 0);
    free(calls);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    printf("test_typed_entries:\n");
    RUN(test_type_and_role_derived);
    RUN(test_ingest_typed);
    return 0;
}
