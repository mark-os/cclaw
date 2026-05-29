#include "context.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

static void test_truncate_result_small(void) {
    /* Small content — no truncation */
    char *r = truncate_result("hello world", 0);
    assert(r != NULL);
    assert(strcmp(r, "hello world") == 0);
    free(r);
    printf("  PASS test_truncate_result_small\n");
}

static void test_truncate_result_null(void) {
    char *r = truncate_result(NULL, 0);
    assert(r == NULL);
    printf("  PASS test_truncate_result_null\n");
}

static void test_truncate_result_lines(void) {
    /* Create 2500 short lines (under 50KB but over 2000 lines) */
    size_t cap = 2500 * 12;
    char *big = malloc(cap + 1);
    char *p = big;
    for (int i = 0; i < 2500; i++)
        p += sprintf(p, "line %04d\n", i);

    char *r = truncate_result(big, 0);
    assert(r != NULL);
    assert(strlen(r) < strlen(big));
    assert(strstr(r, "[truncated") != NULL);
    free(r);
    free(big);
    printf("  PASS test_truncate_result_lines\n");
}

static void test_truncate_result_bytes(void) {
    /* Create content over 50KB but few lines */
    size_t sz = 60 * 1024;
    char *big = malloc(sz + 1);
    memset(big, 'X', sz);
    big[sz] = '\0';

    char *r = truncate_result(big, 0);
    assert(r != NULL);
    assert(strlen(r) < sz);
    assert(strstr(r, "[truncated") != NULL);
    free(r);
    free(big);
    printf("  PASS test_truncate_result_bytes\n");
}

static void test_truncate_result_explicit_len(void) {
    /* Pass explicit len shorter than actual string */
    char *r = truncate_result("hello world extra", 5);
    assert(r != NULL);
    assert(strcmp(r, "hello") == 0);
    free(r);
    printf("  PASS test_truncate_result_explicit_len\n");
}

static Entry make_entry(int64_t id, Role role, const char *content) {
    Entry e = {0};
    e.id = id;
    e.parent_id = id - 1;
    e.session_id = 1;
    e.message.role = role;
    e.message.content = (char *)content;
    return e;
}

static void test_compaction_entry_stops_walk(void) {
    /* V58: ROLE_COMPACTION entry should be included in context but
     * represents a summary — verify it passes through context_build */
    Config cfg = {0};
    cfg.provider.context_window = 128000;

    Entry entries[3] = {
        make_entry(1, ROLE_COMPACTION, "Summary: user asked about X, agent did Y."),
        make_entry(2, ROLE_USER, "continue"),
        make_entry(3, ROLE_ASSISTANT, "ok"),
    };

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 3, &cfg, &msgs, &count);
    assert(rc == 0);
    /* Compaction entry should appear as system-like content */
    assert(count >= 2);
    context_free(msgs, count);
    printf("  PASS test_compaction_entry_stops_walk\n");
}

static void test_context_build_single_user(void) {
    /* Minimal: just one user message */
    Config cfg = {0};
    cfg.provider.context_window = 128000;

    Entry entries[1] = { make_entry(1, ROLE_USER, "hello") };

    Message *msgs = NULL;
    int count = 0;
    int rc = context_build(entries, 1, &cfg, &msgs, &count);
    assert(rc == 0);
    assert(count == 1);
    assert(msgs[0].role == ROLE_USER);
    assert(strcmp(msgs[0].content, "hello") == 0);
    context_free(msgs, count);
    printf("  PASS test_context_build_single_user\n");
}

static void test_context_estimate_tokens_null_content(void) {
    /* Message with NULL content — should not crash */
    Message m = {.role = ROLE_ASSISTANT, .content = NULL};
    int tokens = context_estimate_tokens(&m);
    /* Should be just overhead (4) */
    assert(tokens == 4);
    printf("  PASS test_context_estimate_tokens_null_content\n");
}

static void test_context_estimate_tokens_with_tool_calls(void) {
    /* Tool calls add to token estimate */
    ToolCall tc = {.id = "c1", .name = "shell_exec", .arguments = "{\"cmd\":\"ls -la\"}"};
    Message m = {.role = ROLE_ASSISTANT, .content = NULL,
                 .tool_calls = &tc, .tool_call_count = 1};
    int tokens = context_estimate_tokens(&m);
    /* Should include tool call text in estimate */
    assert(tokens > 4);
    printf("  PASS test_context_estimate_tokens_with_tool_calls\n");
}

static void test_session_tmp_dir(void) {
    char buf[64];
    session_tmp_dir(42, buf, sizeof(buf));
    assert(strcmp(buf, "/tmp/cclaw-42") == 0);
    printf("  PASS test_session_tmp_dir\n");
}

int main(void) {
    alarm(10);
    printf("test_context_edge:\n");
    test_truncate_result_small();
    test_truncate_result_null();
    test_truncate_result_lines();
    test_truncate_result_bytes();
    test_truncate_result_explicit_len();
    test_compaction_entry_stops_walk();
    test_context_build_single_user();
    test_context_estimate_tokens_null_content();
    test_context_estimate_tokens_with_tool_calls();
    test_session_tmp_dir();
    printf("All context_edge tests passed.\n");
    return 0;
}
