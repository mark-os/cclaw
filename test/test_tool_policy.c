#include "tool_policy.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *notes_policy =
    "{\"rules\":["
    "{\"match\":{\"action\":\"delete\"},\"effect\":\"deny\"},"
    "{\"match\":{\"action\":\"write\"},\"effect\":\"ask\"},"
    "{\"match\":{},\"effect\":\"allow\"}"
    "]}";

static const char *array_policy =
    "{\"rules\":["
    "{\"match\":{\"action\":[\"write\",\"delete\"]},\"effect\":\"deny\"},"
    "{\"match\":{},\"effect\":\"allow\"}"
    "]}";

static const char *wildcard_policy =
    "{\"rules\":["
    "{\"match\":{\"name\":\"*\"},\"effect\":\"ask\"},"
    "{\"match\":{},\"effect\":\"allow\"}"
    "]}";

static sqlite3 *db;

static void test_policy_table(void) {
    static const struct {
        const char *label;
        const char *args;
        const char **policy;   /* indirect: the policies are not constants */
        const char *literal;   /* used when policy is NULL */
        int want;
    } cases[] = {
        { "delete -> DENY",  "{\"action\":\"delete\"}", &notes_policy, NULL, POLICY_DENY },
        { "write -> ASK",    "{\"action\":\"write\"}",  &notes_policy, NULL, POLICY_ASK },
        { "read -> ALLOW",   "{\"action\":\"read\"}",   &notes_policy, NULL, POLICY_ALLOW },
        { "list -> ALLOW",   "{\"action\":\"list\"}",   &notes_policy, NULL, POLICY_ALLOW },
        { "NULL policy -> ALLOW",  "{\"action\":\"delete\"}", NULL, NULL, POLICY_ALLOW },
        { "empty policy -> ALLOW", "{\"action\":\"delete\"}", NULL, "", POLICY_ALLOW },
        /* Malformed non-empty policy fails closed (the old jsmn
         * implementation returned ALLOW here, review-3 F1). */
        { "malformed policy -> ERROR", "{\"action\":\"delete\"}", NULL,
          "not json at all", POLICY_ERROR },
        /* Valid object without a rules key is a semantically empty policy. */
        { "no rules key -> ALLOW", "{\"action\":\"delete\"}", NULL,
          "{\"no_rules\":true}", POLICY_ALLOW },
        /* Mis-shaped policies are ERROR, never a silent allow: each of these
         * previously fell through to ALLOW, turning a typo'd deny policy
         * into no policy at all. */
        { "rules-as-object -> ERROR", "{\"action\":\"delete\"}", NULL,
          "{\"rules\":{\"match\":{},\"effect\":\"deny\"}}", POLICY_ERROR },
        { "rule without match -> ERROR", "{\"action\":\"delete\"}", NULL,
          "{\"rules\":[{\"effect\":\"deny\"}]}", POLICY_ERROR },
        { "unknown effect -> ERROR", "{\"action\":\"delete\"}", NULL,
          "{\"rules\":[{\"match\":{},\"effect\":\"denny\"}]}", POLICY_ERROR },
        { "non-object match -> ERROR", "{\"action\":\"delete\"}", NULL,
          "{\"rules\":[{\"match\":\"*\",\"effect\":\"deny\"}]}", POLICY_ERROR },
        /* Malformed args with a keyed deny -> ERROR, never ALLOW. */
        { "malformed args -> ERROR", "not json", &notes_policy, NULL, POLICY_ERROR },
        { "NULL args -> ERROR",      NULL,       &notes_policy, NULL, POLICY_ERROR },
        { "array: write -> DENY",  "{\"action\":\"write\"}",  &array_policy, NULL, POLICY_DENY },
        { "array: delete -> DENY", "{\"action\":\"delete\"}", &array_policy, NULL, POLICY_DENY },
        { "array: read -> ALLOW",  "{\"action\":\"read\"}",   &array_policy, NULL, POLICY_ALLOW },
        { "wildcard: name present -> ASK",
          "{\"action\":\"read\",\"name\":\"foo.txt\"}", &wildcard_policy, NULL, POLICY_ASK },
        { "wildcard: name absent -> ALLOW",
          "{\"action\":\"read\"}", &wildcard_policy, NULL, POLICY_ALLOW },
        /* A non-text value never string-matches a keyed rule, but wildcard
         * presence still sees it. */
        { "non-text value doesn't string-match", "{\"action\":7}", &notes_policy, NULL, POLICY_ALLOW },
        { "wildcard sees non-text value", "{\"name\":7}", &wildcard_policy, NULL, POLICY_ASK },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const char *pol = cases[i].policy ? *cases[i].policy : cases[i].literal;
        int got = policy_eval(db, cases[i].args, pol);
        if (got != cases[i].want)
            printf("    row %zu (%s): got %d want %d\n", i, cases[i].label, got, cases[i].want);
        assert(got == cases[i].want);
    }
}

/* Padded/huge args still hit the keyed deny: the jsmn 128-token bypass
 * (review-2 F10) must not come back. Deep nesting likewise. */
static void test_policy_big_args(void) {
    char big[8192];
    int pos = snprintf(big, sizeof(big), "{\"action\":\"delete\"");
    for (int i = 0; i < 300; i++)
        pos += snprintf(big + pos, sizeof(big) - (size_t)pos, ",\"pad%d\":\"x\"", i);
    snprintf(big + pos, sizeof(big) - (size_t)pos, "}");
    assert(policy_eval(db, big, notes_policy) == POLICY_DENY);

    char nested[4096];
    pos = snprintf(nested, sizeof(nested), "{\"action\":\"write\",\"blob\":");
    for (int i = 0; i < 40; i++)
        pos += snprintf(nested + pos, sizeof(nested) - (size_t)pos, "{\"a\":");
    pos += snprintf(nested + pos, sizeof(nested) - (size_t)pos, "1");
    for (int i = 0; i < 40; i++)
        pos += snprintf(nested + pos, sizeof(nested) - (size_t)pos, "}");
    snprintf(nested + pos, sizeof(nested) - (size_t)pos, "}");
    assert(policy_eval(db, nested, notes_policy) == POLICY_ASK);
}

int main(void) {
    TEST_INIT();
    printf("test_tool_policy:\n");
    db = test_db_open(":memory:");
    assert(db);
    RUN(test_policy_table);
    RUN(test_policy_big_args);
    db_close(db);
    return 0;
}
