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

/* T49/V8: Cut never separates assistant+tool_calls from its tool results */
static void test_v8_multi_tool_calls_kept_together(void) {
    TEST(v8_multi_tool_calls_kept_together);
    /* Scenario: assistant makes 2 tool calls, followed by 2 tool results,
     * then a user msg. Budget only fits the user msg + maybe one group.
     * The assistant+tools group must stay together or be dropped entirely. */
    Config cfg = {0};
    cfg.max_history_tokens = 40; /* very tight budget */

    char big_args[200];
    memset(big_args, 'Z', 199);
    big_args[199] = '\0';

    ToolCall tcs[2] = {
        { .id = "c1", .name = "file_read", .arguments = big_args },
        { .id = "c2", .name = "shell_exec", .arguments = big_args },
    };
    ToolResult tr1 = { .tool_call_id = "c1", .content = "content1" };
    ToolResult tr2 = { .tool_call_id = "c2", .content = "content2" };

    Entry entries[5] = {
        make_entry(1, ROLE_USER, "do stuff"),
        make_entry(2, ROLE_ASSISTANT, NULL),
        make_entry(3, ROLE_TOOL, NULL),
        make_entry(4, ROLE_TOOL, NULL),
        make_entry(5, ROLE_USER, "ok"),
    };
    entries[1].message.tool_calls = tcs;
    entries[1].message.tool_call_count = 2;
    entries[2].message.tool_result = &tr1;
    entries[3].message.tool_result = &tr2;

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 5, &cfg, &msgs, &count);
    if (rc != 0) { FAIL("returned error"); return; }

    /* Verify: no tool result appears without its preceding assistant */
    for (int i = 0; i < count; i++) {
        if (msgs[i].role == ROLE_TOOL) {
            if (i == 0 || (msgs[i-1].role != ROLE_ASSISTANT && msgs[i-1].role != ROLE_TOOL)) {
                FAIL("orphaned tool result — V8 violated");
                context_free(msgs, count);
                return;
            }
        }
    }
    context_free(msgs, count);
    PASS();
}

/* T49/V8: Cut lands before a user message, not mid-conversation */
static void test_v8_cut_at_user_boundary(void) {
    TEST(v8_cut_at_user_boundary);
    /* Budget fits last 2 messages but not all 5.
     * Cut must land before a user msg (or system msg). */
    Config cfg = {0};
    cfg.max_history_tokens = 30;

    Entry entries[5] = {
        make_entry(1, ROLE_USER, "first question with some extra text padding here"),
        make_entry(2, ROLE_ASSISTANT, "first answer with some extra text padding here"),
        make_entry(3, ROLE_USER, "second question"),
        make_entry(4, ROLE_ASSISTANT, "second answer"),
        make_entry(5, ROLE_USER, "third"),
    };

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 5, &cfg, &msgs, &count);
    if (rc != 0) { FAIL("returned error"); return; }

    /* First real message after cutoff notice must be user or system */
    int start = 0;
    if (count > 0 && msgs[0].role == ROLE_SYSTEM && strstr(msgs[0].content, "truncated"))
        start = 1;

    if (start < count && msgs[start].role != ROLE_USER && msgs[start].role != ROLE_SYSTEM) {
        FAIL("cut did not land at user/system boundary");
        context_free(msgs, count);
        return;
    }
    context_free(msgs, count);
    PASS();
}

/* T49/V8: When tool group is at the boundary, it's dropped entirely rather than split */
static void test_v8_tool_group_at_boundary_dropped(void) {
    TEST(v8_tool_group_at_boundary_dropped);
    /* Budget fits user msg at end but not the preceding tool group.
     * The tool group must be dropped entirely — no partial inclusion. */
    Config cfg = {0};
    cfg.max_history_tokens = 20; /* only fits ~80 chars */

    ToolCall tc = { .id = "c1", .name = "shell_exec", .arguments = "{\"cmd\":\"echo hi\"}" };
    ToolResult tr = { .tool_call_id = "c1", .content = "hi" };

    char big[300];
    memset(big, 'W', 299);
    big[299] = '\0';

    Entry entries[4] = {
        make_entry(1, ROLE_ASSISTANT, NULL),
        make_entry(2, ROLE_TOOL, NULL),
        make_entry(3, ROLE_USER, big),  /* big user msg that barely fits */
        make_entry(4, ROLE_USER, "end"),
    };
    entries[0].message.tool_calls = &tc;
    entries[0].message.tool_call_count = 1;
    entries[1].message.tool_result = &tr;

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 4, &cfg, &msgs, &count);
    if (rc != 0) { FAIL("returned error"); return; }

    /* No assistant-with-tool-calls should appear without its tool results */
    for (int i = 0; i < count; i++) {
        if (msgs[i].role == ROLE_ASSISTANT && msgs[i].tool_calls) {
            /* Next msg must be tool */
            if (i + 1 >= count || msgs[i + 1].role != ROLE_TOOL) {
                FAIL("assistant with tool_calls but no following tool result");
                context_free(msgs, count);
                return;
            }
        }
    }
    context_free(msgs, count);
    PASS();
}

/* T49/V7: Included messages fit within token budget */
static void test_v7_result_within_budget(void) {
    TEST(v7_result_within_budget);
    Config cfg = {0};
    cfg.max_history_tokens = 50;

    Entry entries[6] = {
        make_entry(1, ROLE_USER, "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj"),
        make_entry(2, ROLE_ASSISTANT, "aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj"),
        make_entry(3, ROLE_USER, "kkkk llll mmmm nnnn oooo pppp qqqq rrrr ssss tttt"),
        make_entry(4, ROLE_ASSISTANT, "short"),
        make_entry(5, ROLE_USER, "hi"),
        make_entry(6, ROLE_ASSISTANT, "hey"),
    };

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 6, &cfg, &msgs, &count);
    if (rc != 0) { FAIL("returned error"); return; }

    /* Sum tokens of included messages (skip cutoff notice) */
    int total = 0;
    int start = 0;
    if (count > 0 && msgs[0].role == ROLE_SYSTEM && strstr(msgs[0].content, "truncated"))
        start = 1;
    for (int i = start; i < count; i++)
        total += context_estimate_tokens(&msgs[i]);

    if (total > 50) {
        FAIL("included messages exceed budget");
        context_free(msgs, count);
        return;
    }
    context_free(msgs, count);
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
    test_v8_multi_tool_calls_kept_together();
    test_v8_cut_at_user_boundary();
    test_v8_tool_group_at_boundary_dropped();
    test_v7_result_within_budget();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
