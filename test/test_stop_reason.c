#include <stdio.h>
#include <string.h>
#include "llm.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)
#define ASSERT_EQ(got, want) do { \
    if ((got) != (want)) { FAIL(#got " != " #want); return; } \
} while(0)

static void test_stop_variants(void) {
    TEST(stop_variants);
    ASSERT_EQ(map_stop_reason("stop"), STOP_REASON_STOP);
    ASSERT_EQ(map_stop_reason("end"), STOP_REASON_STOP);
    ASSERT_EQ(map_stop_reason("end_turn"), STOP_REASON_STOP);
    ASSERT_EQ(map_stop_reason(NULL), STOP_REASON_STOP);
    PASS();
}

static void test_length_variants(void) {
    TEST(length_variants);
    ASSERT_EQ(map_stop_reason("length"), STOP_REASON_LENGTH);
    ASSERT_EQ(map_stop_reason("max_tokens"), STOP_REASON_LENGTH);
    PASS();
}

static void test_tool_use_variants(void) {
    TEST(tool_use_variants);
    ASSERT_EQ(map_stop_reason("tool_calls"), STOP_REASON_TOOL_USE);
    ASSERT_EQ(map_stop_reason("function_call"), STOP_REASON_TOOL_USE);
    ASSERT_EQ(map_stop_reason("tool_use"), STOP_REASON_TOOL_USE);
    PASS();
}

static void test_error_variants(void) {
    TEST(error_variants);
    ASSERT_EQ(map_stop_reason("content_filter"), STOP_REASON_ERROR);
    ASSERT_EQ(map_stop_reason("network_error"), STOP_REASON_ERROR);
    ASSERT_EQ(map_stop_reason("something_unknown"), STOP_REASON_ERROR);
    PASS();
}

int main(void) {
    printf("--- test_stop_reason ---\n");
    test_stop_variants();
    test_length_variants();
    test_tool_use_variants();
    test_error_variants();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
