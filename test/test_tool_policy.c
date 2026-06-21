#include "tool_policy.h"
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
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* notes policy tests */
    CHECK(policy_eval("{\"action\":\"delete\"}", notes_policy) == POLICY_DENY,
          "delete -> DENY");
    CHECK(policy_eval("{\"action\":\"write\"}", notes_policy) == POLICY_ASK,
          "write -> ASK");
    CHECK(policy_eval("{\"action\":\"read\"}", notes_policy) == POLICY_ALLOW,
          "read -> ALLOW (catch-all)");
    CHECK(policy_eval("{\"action\":\"list\"}", notes_policy) == POLICY_ALLOW,
          "list -> ALLOW (catch-all)");

    /* NULL policy -> ALLOW */
    CHECK(policy_eval("{\"action\":\"delete\"}", NULL) == POLICY_ALLOW,
          "NULL policy -> ALLOW");

    /* Empty string policy -> ALLOW */
    CHECK(policy_eval("{\"action\":\"delete\"}", "") == POLICY_ALLOW,
          "empty policy -> ALLOW");

    /* Malformed policy -> ALLOW (fail-open) */
    CHECK(policy_eval("{\"action\":\"delete\"}", "not json at all") == POLICY_ALLOW,
          "malformed policy -> ALLOW");
    CHECK(policy_eval("{\"action\":\"delete\"}", "{\"no_rules\":true}") == POLICY_ALLOW,
          "no rules array -> ALLOW");

    /* Array match: action in [write, delete] -> DENY */
    CHECK(policy_eval("{\"action\":\"write\"}", array_policy) == POLICY_DENY,
          "array match: write -> DENY");
    CHECK(policy_eval("{\"action\":\"delete\"}", array_policy) == POLICY_DENY,
          "array match: delete -> DENY");
    CHECK(policy_eval("{\"action\":\"read\"}", array_policy) == POLICY_ALLOW,
          "array match: read -> ALLOW (catch-all)");

    /* Wildcard: name present -> ASK, name absent -> ALLOW (catch-all) */
    CHECK(policy_eval("{\"action\":\"read\",\"name\":\"foo.txt\"}", wildcard_policy) == POLICY_ASK,
          "wildcard: name present -> ASK");
    CHECK(policy_eval("{\"action\":\"read\"}", wildcard_policy) == POLICY_ALLOW,
          "wildcard: name absent -> ALLOW (catch-all)");

    printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
