/* Test: Buf growable buffer and template_render */
#define _POSIX_C_SOURCE 200809L
#include "buf.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_append_growth(void) {
    Buf b = {0};
    /* Force several growth boundaries: initial cap is 256. */
    for (int i = 0; i < 1000; i++) buf_append_char(&b, 'x');
    assert(b.len == 1000);
    char *s = buf_take(&b);
    assert(s);
    assert(strlen(s) == 1000);
    for (int i = 0; i < 1000; i++) assert(s[i] == 'x');
    free(s);
    printf("  append_growth... PASS\n");
}

static void test_append_str_and_char(void) {
    Buf b = {0};
    buf_append_str(&b, "hello ");
    buf_append_str(&b, "world");
    buf_append_char(&b, '!');
    char *s = buf_take(&b);
    assert(s && strcmp(s, "hello world!") == 0);
    free(s);
    printf("  append_str_and_char... PASS\n");
}

static void test_appendf_short_and_long(void) {
    Buf b = {0};
    buf_appendf(&b, "n=%d ", 42);
    /* Force the vsnprintf retry path (>256 bytes in one call). */
    char pad[512];
    memset(pad, 'a', sizeof(pad) - 1);
    pad[sizeof(pad) - 1] = '\0';
    buf_appendf(&b, "%s", pad);
    char *s = buf_take(&b);
    assert(s);
    assert(strncmp(s, "n=42 ", 5) == 0);
    assert(strlen(s) == 5 + strlen(pad));
    free(s);
    printf("  appendf_short_and_long... PASS\n");
}

static void test_take_empty(void) {
    Buf b = {0};
    char *s = buf_take(&b);
    assert(s && s[0] == '\0');
    free(s);
    printf("  take_empty... PASS\n");
}

static void test_take_resets_buf(void) {
    Buf b = {0};
    buf_append_str(&b, "abc");
    char *s = buf_take(&b);
    free(s);
    assert(b.data == NULL && b.len == 0 && b.cap == 0 && b.oom == 0);
    /* Buf is reusable after take. */
    buf_append_str(&b, "def");
    s = buf_take(&b);
    assert(s && strcmp(s, "def") == 0);
    free(s);
    printf("  take_resets_buf... PASS\n");
}

static void test_template_render_multi_var(void) {
    TemplateVar vars[] = {
        {"{date}", "2026-07-02"},
        {"{agent_name}", "assistant"},
    };
    char *out = template_render("Today is {date}, I am {agent_name}.", vars, 2);
    assert(out && strcmp(out, "Today is 2026-07-02, I am assistant.") == 0);
    free(out);
    printf("  template_render_multi_var... PASS\n");
}

static void test_template_render_no_match_passthrough(void) {
    TemplateVar vars[] = { {"{date}", "2026-07-02"} };
    char *out = template_render("no vars here, just {curly} braces", vars, 1);
    assert(out && strcmp(out, "no vars here, just {curly} braces") == 0);
    free(out);
    printf("  template_render_no_match_passthrough... PASS\n");
}

static void test_template_render_var_at_end(void) {
    TemplateVar vars[] = { {"{date}", "2026-07-02"} };
    char *out = template_render("date: {date}", vars, 1);
    assert(out && strcmp(out, "date: 2026-07-02") == 0);
    free(out);
    printf("  template_render_var_at_end... PASS\n");
}

static void test_template_render_repeated_var(void) {
    TemplateVar vars[] = { {"{x}", "Y"} };
    char *out = template_render("{x}{x}{x}", vars, 1);
    assert(out && strcmp(out, "YYY") == 0);
    free(out);
    printf("  template_render_repeated_var... PASS\n");
}

int main(void) {
    printf("test_buf:\n");
    test_append_growth();
    test_append_str_and_char();
    test_appendf_short_and_long();
    test_take_empty();
    test_take_resets_buf();
    test_template_render_multi_var();
    test_template_render_no_match_passthrough();
    test_template_render_var_at_end();
    test_template_render_repeated_var();
    printf("all buf tests passed\n");
    return 0;
}
