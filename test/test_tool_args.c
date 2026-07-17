/* Unit tests for tool_args.c — SQLite-JSON1-backed argument extraction. */
#include "tool_args.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    TEST_INIT();
    printf("test_tool_args:\n");
    sqlite3 *db = test_db_open(":memory:");
    assert(db);

    const char *args =
        "{\"cmd\":\"echo \\\"hi\\\"\",\"n\":7,\"flag\":true,"
        "\"edits\":[{\"oldText\":\"a\",\"newText\":\"b\"}],\"obj\":{\"k\":1}}";

    /* Validation: object required, garbage and scalars rejected. */
    assert(tool_args_valid_object(db, args) == 1);
    assert(tool_args_valid_object(db, "{}") == 1);
    assert(tool_args_valid_object(db, "not json") == 0);
    assert(tool_args_valid_object(db, "[1,2]") == 0);
    assert(tool_args_valid_object(db, "\"str\"") == 0);
    assert(tool_args_valid_object(db, NULL) == 0);
    /* No token cap: deeply padded args still validate (the old jsmn
     * 64/128-token budgets are gone). */
    {
        char big[8192] = "{";
        size_t off = 1;
        for (int i = 0; i < 300; i++)
            off += (size_t)snprintf(big + off, sizeof(big) - off,
                                    "\"k%d\":%d,", i, i);
        snprintf(big + off, sizeof(big) - off, "\"last\":1}");
        assert(tool_args_valid_object(db, big) == 1);
        char *v = tool_args_str(db, big, "k299");
        assert(v == NULL); /* number, not text */
        assert(tool_args_int(db, big, "k299", -1) == 299);
    }

    /* String extraction unescapes; wrong type / absent → NULL. */
    char *s = tool_args_str(db, args, "cmd");
    assert(s && strcmp(s, "echo \"hi\"") == 0);
    free(s);
    assert(tool_args_str(db, args, "n") == NULL);
    assert(tool_args_str(db, args, "missing") == NULL);
    assert(tool_args_str(db, "garbage", "cmd") == NULL);

    /* Int / bool with defaults. */
    assert(tool_args_int(db, args, "n", -1) == 7);
    assert(tool_args_int(db, args, "cmd", -1) == -1);
    assert(tool_args_int(db, args, "missing", 42) == 42);
    assert(tool_args_bool(db, args, "flag", 0) == 1);
    assert(tool_args_bool(db, args, "missing", 1) == 1);
    assert(tool_args_bool(db, args, "n", 0) == 0);

    /* Sub-blob extraction returns JSON text. */
    char *j = tool_args_json(db, args, "edits");
    assert(j && j[0] == '[' && strstr(j, "oldText"));
    free(j);
    j = tool_args_json(db, args, "obj");
    assert(j && j[0] == '{');
    free(j);
    assert(tool_args_json(db, args, "cmd") == NULL);

    db_close(db);
    printf("  PASS tool_args\nAll tool_args tests passed.\n");
    return 0;
}
