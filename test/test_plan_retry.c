#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* V45: replicate is_plan_only logic for unit testing */
static int is_plan_only(const char *content) {
    if (!content || !*content) return 0;
    int has_bullets = (strstr(content, "\n- ") || strstr(content, "\n* ") ||
                       strstr(content, "\n1.") || strstr(content, "\n1)"));
    if (!has_bullets) return 0;
    static const char *promises[] = {
        "I'll ", "I will ", "Let me ", "I'm going to ",
        "Here's my plan", "Here is my plan", "I can ",
        "I'll:", "I will:", "Let me:", NULL
    };
    for (int i = 0; promises[i]; i++) {
        if (strstr(content, promises[i])) return 1;
    }
    return 0;
}

static void test_plan_only_bullets_and_promise(void) {
    TEST(plan_only_bullets_and_promise);
    const char *text = "Here's my plan:\n- Read the file\n- Fix the bug\n- Run tests";
    if (!is_plan_only(text)) { FAIL("should detect plan-only"); return; }
    PASS();
}

static void test_plan_only_numbered_list(void) {
    TEST(plan_only_numbered_list);
    const char *text = "I'll do the following:\n1. First step\n2. Second step";
    if (!is_plan_only(text)) { FAIL("should detect numbered plan"); return; }
    PASS();
}

static void test_plan_only_star_bullets(void) {
    TEST(plan_only_star_bullets);
    const char *text = "Let me work through this:\n* Step one\n* Step two";
    if (!is_plan_only(text)) { FAIL("should detect star bullets"); return; }
    PASS();
}

static void test_not_plan_no_bullets(void) {
    TEST(not_plan_no_bullets);
    const char *text = "I'll fix the bug now.";
    if (is_plan_only(text)) { FAIL("no bullets, should not be plan-only"); return; }
    PASS();
}

static void test_not_plan_no_promise(void) {
    TEST(not_plan_no_promise);
    const char *text = "The results are:\n- Item A\n- Item B\n- Item C";
    if (is_plan_only(text)) { FAIL("no promise verb, should not be plan-only"); return; }
    PASS();
}

static void test_not_plan_empty(void) {
    TEST(not_plan_empty);
    if (is_plan_only(NULL)) { FAIL("NULL should not be plan-only"); return; }
    if (is_plan_only("")) { FAIL("empty should not be plan-only"); return; }
    PASS();
}

static void test_not_plan_concrete_answer(void) {
    TEST(not_plan_concrete_answer);
    const char *text = "Done. The file has been written to disk.";
    if (is_plan_only(text)) { FAIL("concrete answer should not be plan-only"); return; }
    PASS();
}

static void test_plan_only_will_colon(void) {
    TEST(plan_only_will_colon);
    const char *text = "I will:\n- Read config\n- Update the value";
    if (!is_plan_only(text)) { FAIL("should detect I will: pattern"); return; }
    PASS();
}

int main(void) {
    printf("test_plan_retry:\n");
    test_plan_only_bullets_and_promise();
    test_plan_only_numbered_list();
    test_plan_only_star_bullets();
    test_not_plan_no_bullets();
    test_not_plan_no_promise();
    test_not_plan_empty();
    test_not_plan_concrete_answer();
    test_plan_only_will_colon();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
