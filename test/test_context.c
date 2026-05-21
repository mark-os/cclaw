#include <stdio.h>
#include <string.h>
#include "context.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static Entry make_entry(int64_t id, Role role, const char *content) {
    Entry e = {0};
    e.id = id;
    e.parent_id = id - 1;
    e.session_id = 1;
    e.message.role = role;
    e.message.content = (char *)content;
    return e;
}

static void test_all_fits(void) {
    TEST(all_fits);
    Entry entries[3] = {
        make_entry(1, ROLE_SYSTEM, "You are helpful."),
        make_entry(2, ROLE_USER, "Hello"),
        make_entry(3, ROLE_ASSISTANT, "Hi there!"),
    };
    Config cfg = {0};
    cfg.provider.context_window = 128000;

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 3, &cfg, &msgs, &count);
    if (rc != 0) { FAIL("returned error"); return; }
    if (count != 3) { FAIL("wrong count"); context_free(msgs, count); return; }
    if (msgs[0].role != ROLE_SYSTEM) { FAIL("no system msg"); context_free(msgs, count); return; }
    if (strcmp(msgs[0].content, "You are helpful.") != 0) {
        FAIL("wrong system content"); context_free(msgs, count); return;
    }
    context_free(msgs, count);
    PASS();
}

static void test_truncation_with_cutoff(void) {
    TEST(truncation_with_cutoff);
    /* Use tiny context window to force truncation */
    Config cfg = {0};
    cfg.provider.context_window = 100; /* 60 token budget */

    /* Create entries that exceed budget */
    char big[300];
    memset(big, 'A', 299);
    big[299] = '\0';

    Entry entries[3] = {
        make_entry(1, ROLE_USER, big),       /* ~75 tokens — exceeds budget alone */
        make_entry(2, ROLE_ASSISTANT, "ok"),
        make_entry(3, ROLE_USER, "short"),
    };

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 3, &cfg, &msgs, &count);
    if (rc != 0) { FAIL("returned error"); return; }
    /* Should have cutoff notice + some messages */
    if (count < 2) { FAIL("too few messages"); context_free(msgs, count); return; }
    if (msgs[0].role != ROLE_SYSTEM) { FAIL("first should be system cutoff"); context_free(msgs, count); return; }
    if (strstr(msgs[0].content, "truncated") == NULL) {
        FAIL("cutoff notice missing"); context_free(msgs, count); return;
    }
    context_free(msgs, count);
    PASS();
}

static void test_v8_no_mid_tool_call_cut(void) {
    TEST(v8_no_mid_tool_call_cut);
    /* Budget that can fit tool result + user but not the assistant+tool group */
    Config cfg = {0};
    cfg.provider.context_window = 200; /* 120 token budget */

    ToolCall tc = { .id = "call_1", .name = "shell_exec", .arguments = "{\"cmd\":\"ls\"}" };
    ToolResult tr = { .tool_call_id = "call_1", .content = "file1.txt\nfile2.txt" };

    char big[400];
    memset(big, 'B', 399);
    big[399] = '\0';

    Entry entries[4] = {
        make_entry(1, ROLE_USER, big),  /* ~100 tokens */
        make_entry(2, ROLE_ASSISTANT, NULL),
        make_entry(3, ROLE_TOOL, NULL),
        make_entry(4, ROLE_USER, "what files?"),
    };
    entries[1].message.tool_calls = &tc;
    entries[1].message.tool_call_count = 1;
    entries[2].message.tool_result = &tr;

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 4, &cfg, &msgs, &count);
    if (rc != 0) { FAIL("returned error"); return; }

    /* V8: should not include tool result without its assistant msg.
     * Either include the whole group (assistant+tool+user) or skip to next valid boundary. */
    int has_tool_without_assistant = 0;
    for (int i = 0; i < count; i++) {
        if (msgs[i].role == ROLE_TOOL) {
            /* Check that preceding msg is assistant with tool_calls */
            if (i == 0 || msgs[i-1].role != ROLE_ASSISTANT) {
                has_tool_without_assistant = 1;
            }
        }
    }
    if (has_tool_without_assistant) {
        FAIL("tool result without assistant — V8 violated");
        context_free(msgs, count);
        return;
    }
    context_free(msgs, count);
    PASS();
}

static void test_estimate_tokens(void) {
    TEST(estimate_tokens);
    Message m = { .role = ROLE_USER, .content = "Hello world" }; /* 11 chars */
    int tokens = context_estimate_tokens(&m);
    /* 11/4 + 4 overhead = 6 */
    if (tokens != 6) { FAIL("wrong estimate"); return; }
    PASS();
}

static void test_max_history_tokens_override(void) {
    TEST(max_history_tokens_override);
    /* Set max_history_tokens explicitly — should override 60% default */
    Config cfg = {0};
    cfg.provider.context_window = 128000; /* 60% = 76800 */
    cfg.max_history_tokens = 20; /* tiny budget forces truncation */

    char big[200];
    memset(big, 'X', 199);
    big[199] = '\0';

    Entry entries[3] = {
        make_entry(1, ROLE_USER, big),       /* ~54 tokens — exceeds budget of 20 */
        make_entry(2, ROLE_ASSISTANT, "ok"),
        make_entry(3, ROLE_USER, "hi"),
    };

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 3, &cfg, &msgs, &count);
    if (rc != 0) { FAIL("returned error"); return; }
    /* Budget is 20 tokens — should truncate the big message */
    if (msgs[0].role != ROLE_SYSTEM || strstr(msgs[0].content, "truncated") == NULL) {
        FAIL("expected truncation with small max_history_tokens");
        context_free(msgs, count);
        return;
    }
    context_free(msgs, count);
    PASS();
}

static void test_empty_input(void) {
    TEST(empty_input);
    Config cfg = {0};
    cfg.provider.context_window = 128000;
    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(NULL, 0, &cfg, &msgs, &count);
    if (rc != -1) { FAIL("should fail on NULL"); return; }
    PASS();
}

int main(void) {
    printf("--- test_context ---\n");
    test_all_fits();
    test_truncation_with_cutoff();
    test_v8_no_mid_tool_call_cut();
    test_estimate_tokens();
    test_max_history_tokens_override();
    test_empty_input();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
