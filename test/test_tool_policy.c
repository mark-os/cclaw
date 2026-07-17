#include "tool_policy.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, label) do { \
    if (!(cond)) { printf("FAIL: %s\n", label); failures++; } \
    else { printf("PASS: %s\n", label); } \
} while(0)

/* The notes policy from the plan */
static const char *notes_policy =
    "{\"rules\":["
    "{\"match\":{\"action\":\"delete\"},\"effect\":\"deny\"},"
    "{\"match\":{\"action\":\"write\"},\"effect\":\"ask\"},"
    "{\"match\":{},\"effect\":\"allow\"}"
    "]}";

/* Array match policy */
static const char *array_policy =
    "{\"rules\":["
    "{\"match\":{\"action\":[\"write\",\"delete\"]},\"effect\":\"deny\"},"
    "{\"match\":{},\"effect\":\"allow\"}"
    "]}";

/* Wildcard match policy */
static const char *wildcard_policy =
    "{\"rules\":["
    "{\"match\":{\"name\":\"*\"},\"effect\":\"ask\"},"
    "{\"match\":{},\"effect\":\"allow\"}"
    "]}";

int main(void) {
    TEST_INIT();
    sqlite3 *db = test_db_open(":memory:");
    if (!db) { printf("FAIL: db open\n"); return 1; }

    /* notes policy tests */
    CHECK(policy_eval(db, "{\"action\":\"delete\"}", notes_policy) == POLICY_DENY,
          "delete -> DENY");
    CHECK(policy_eval(db, "{\"action\":\"write\"}", notes_policy) == POLICY_ASK,
          "write -> ASK");
    CHECK(policy_eval(db, "{\"action\":\"read\"}", notes_policy) == POLICY_ALLOW,
          "read -> ALLOW (catch-all)");
    CHECK(policy_eval(db, "{\"action\":\"list\"}", notes_policy) == POLICY_ALLOW,
          "list -> ALLOW (catch-all)");

    /* NULL policy -> ALLOW */
    CHECK(policy_eval(db, "{\"action\":\"delete\"}", NULL) == POLICY_ALLOW,
          "NULL policy -> ALLOW");

    /* Empty string policy -> ALLOW */
    CHECK(policy_eval(db, "{\"action\":\"delete\"}", "") == POLICY_ALLOW,
          "empty policy -> ALLOW");

    /* Malformed non-empty policy -> ERROR (fail closed — the old jsmn
     * implementation returned ALLOW here, review-3 F1). */
    CHECK(policy_eval(db, "{\"action\":\"delete\"}", "not json at all") == POLICY_ERROR,
          "malformed policy -> ERROR (blocked)");
    /* Valid object without a rules key is a semantically empty policy. */
    CHECK(policy_eval(db, "{\"action\":\"delete\"}", "{\"no_rules\":true}") == POLICY_ALLOW,
          "no rules key -> ALLOW");

    /* Mis-shaped policies are ERROR, never a silent allow: rules of the
     * wrong type, a rule without a match object, an unknown effect. Each of
     * these previously fell through to ALLOW (skipped rule / has_rules=0),
     * turning a typo'd deny policy into no policy at all. */
    CHECK(policy_eval(db, "{\"action\":\"delete\"}",
                      "{\"rules\":{\"match\":{},\"effect\":\"deny\"}}") == POLICY_ERROR,
          "rules-as-object -> ERROR (blocked)");
    CHECK(policy_eval(db, "{\"action\":\"delete\"}",
                      "{\"rules\":[{\"effect\":\"deny\"}]}") == POLICY_ERROR,
          "rule without match -> ERROR (blocked)");
    CHECK(policy_eval(db, "{\"action\":\"delete\"}",
                      "{\"rules\":[{\"match\":{},\"effect\":\"denny\"}]}") == POLICY_ERROR,
          "unknown effect -> ERROR (blocked)");
    CHECK(policy_eval(db, "{\"action\":\"delete\"}",
                      "{\"rules\":[{\"match\":\"*\",\"effect\":\"deny\"}]}") == POLICY_ERROR,
          "non-object match -> ERROR (blocked)");

    /* Malformed args with a keyed deny -> ERROR, never ALLOW. */
    CHECK(policy_eval(db, "not json", notes_policy) == POLICY_ERROR,
          "malformed args -> ERROR (blocked)");
    CHECK(policy_eval(db, NULL, notes_policy) == POLICY_ERROR,
          "NULL args -> ERROR (blocked)");

    /* Array match: action in [write, delete] -> DENY */
    CHECK(policy_eval(db, "{\"action\":\"write\"}", array_policy) == POLICY_DENY,
          "array match: write -> DENY");
    CHECK(policy_eval(db, "{\"action\":\"delete\"}", array_policy) == POLICY_DENY,
          "array match: delete -> DENY");
    CHECK(policy_eval(db, "{\"action\":\"read\"}", array_policy) == POLICY_ALLOW,
          "array match: read -> ALLOW (catch-all)");

    /* Wildcard: name present -> ASK, name absent -> ALLOW (catch-all) */
    CHECK(policy_eval(db, "{\"action\":\"read\",\"name\":\"foo.txt\"}", wildcard_policy) == POLICY_ASK,
          "wildcard: name present -> ASK");
    CHECK(policy_eval(db, "{\"action\":\"read\"}", wildcard_policy) == POLICY_ALLOW,
          "wildcard: name absent -> ALLOW (catch-all)");

    /* Padded/huge args parse fine and still hit the keyed deny — the jsmn
     * 128-token bypass (review-2 F10) ceases to exist. */
    {
        char big[8192];
        int pos = snprintf(big, sizeof(big), "{\"action\":\"delete\"");
        for (int i = 0; i < 300; i++)
            pos += snprintf(big + pos, sizeof(big) - (size_t)pos,
                            ",\"pad%d\":\"x\"", i);
        snprintf(big + pos, sizeof(big) - (size_t)pos, "}");
        CHECK(policy_eval(db, big, notes_policy) == POLICY_DENY,
              "padded args still hit keyed deny");
    }

    /* Deeply nested args (would blow a token budget) match normally. */
    {
        char nested[4096];
        int pos = snprintf(nested, sizeof(nested), "{\"action\":\"write\",\"blob\":");
        for (int i = 0; i < 40; i++)
            pos += snprintf(nested + pos, sizeof(nested) - (size_t)pos, "{\"a\":");
        pos += snprintf(nested + pos, sizeof(nested) - (size_t)pos, "1");
        for (int i = 0; i < 40; i++)
            pos += snprintf(nested + pos, sizeof(nested) - (size_t)pos, "}");
        snprintf(nested + pos, sizeof(nested) - (size_t)pos, "}");
        CHECK(policy_eval(db, nested, notes_policy) == POLICY_ASK,
              "nested args still hit keyed ask");
    }

    /* Non-text arg value never string-matches a keyed rule (falls through
     * to the catch-all), but wildcard presence still sees it. */
    CHECK(policy_eval(db, "{\"action\":7}", notes_policy) == POLICY_ALLOW,
          "non-text value doesn't string-match");
    CHECK(policy_eval(db, "{\"name\":7}", wildcard_policy) == POLICY_ASK,
          "wildcard sees non-text value");

    db_close(db);
    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
