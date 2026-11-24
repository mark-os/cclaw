/* Buf growable buffer and template_render. */
#define _POSIX_C_SOURCE 200809L
#include "buf.h"
#include "test_util.h"
#include <assert.h>

static void test_buf(void) {
    Buf b = {0};
    char *s = buf_take(&b);
    assert(s && s[0] == '\0');
    free(s);

    buf_append_str(&b, "n=");
    buf_appendf(&b, "%d ", 42);
    buf_append_char(&b, '!');
    /* One append past the initial 256-byte cap: growth and the vsnprintf
     * retry path. */
    char pad[512];
    memset(pad, 'a', sizeof(pad) - 1);
    pad[sizeof(pad) - 1] = '\0';
    buf_appendf(&b, "%s", pad);
    s = buf_take(&b);
    assert(s && strncmp(s, "n=42 !", 6) == 0);
    assert(strlen(s) == 6 + strlen(pad));
    free(s);

    /* take resets the Buf, and it is reusable. */
    assert(b.data == NULL && b.len == 0 && b.cap == 0 && b.oom == 0);
    buf_append_str(&b, "def");
    s = buf_take(&b);
    assert(s && strcmp(s, "def") == 0);
    free(s);
}

static void test_template_render(void) {
    TemplateVar vars[] = { {"{date}", "2026-07-02"}, {"{x}", "Y"} };
    static const struct { const char *in, *want; } cases[] = {
        { "Today is {date}, I am {x}.", "Today is 2026-07-02, I am Y." },
        { "no vars here, just {curly} braces", "no vars here, just {curly} braces" },
        { "date: {date}", "date: 2026-07-02" },
        { "{x}{x}{x}", "YYY" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char *out = template_render(cases[i].in, vars, 2);
        if (!out || strcmp(out, cases[i].want) != 0) {
            fprintf(stderr, "row %zu: %s -> %s\n", i, cases[i].in, out ? out : "(null)");
            assert(0);
        }
        free(out);
    }
}

int main(void) {
    TEST_INIT();
    RUN(test_buf);
    RUN(test_template_render);
    return 0;
}
