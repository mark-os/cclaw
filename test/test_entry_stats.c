#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "db.h"
#include "test_util.h"

/* entry_append writes the split columns straight from the Message: model and
 * usage on an assistant entry, tool name, error flag and call id on a result. */
static void test_split_columns_written(void) {
    sqlite3 *db = test_db_open(":memory:");
    assert(db);
    int64_t sid = session_create(db, "test", NULL, -1, 0);

    Message asst = {.role = ROLE_ASSISTANT, .content = "Hello", .stop_reason = STOP_REASON_STOP,
                    .model = "deepseek/deepseek-v4.1-flash", .usage_in = 100, .usage_out = 50};
    int64_t eid = entry_append_with_iteration(db, sid, &asst, 1);
    assert(eid > 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE id=?1"
                             " AND model='deepseek/deepseek-v4.1-flash'"
                             " AND usage_in=100 AND usage_out=50", eid, -1) == 1);

    ToolResult tr = {.tool_call_id = "tc_1", .content = "error: not found"};
    Message tool_msg = {.role = ROLE_TOOL, .tool_result = &tr,
                        .tool_name = "file_read", .is_error = 1};
    int64_t tid = entry_append_with_iteration(db, sid, &tool_msg, 1);
    assert(tid > 0);
    assert(db_scalar_i64(db, "SELECT COUNT(*) FROM entries WHERE id=?1 AND tool_name='file_read'"
                             " AND is_error=1 AND tool_call_id='tc_1'", tid, -1) == 1);
    db_close(db);
}

int main(void) {
    TEST_INIT();
    printf("test_entry_stats:\n");
    RUN(test_split_columns_written);
    return 0;
}
