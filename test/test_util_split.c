/* Test: split_and_trim (shared comma-/whitespace-list tokenizer).
 * Replaced five hand-rolled copies (channel egress_hosts, admin_ids in three
 * places, a strtok_r variant) that had drifted on whether space was a
 * delimiter or just padding. */
#define _POSIX_C_SOURCE 200809L
#include "util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test_util.h"

static void expect(const char *name, const char *input, int max,
                   const char *const *want, int n_want) {
    char *buf = input ? strdup(input) : NULL;
    char *out[32];
    int n = split_and_trim(buf, out, max);
    int ok = (n == n_want);
    if (ok)
        for (int i = 0; i < n; i++)
            if (strcmp(out[i], want[i]) != 0) { ok = 0; break; }
    if (!ok) {
        printf("  %s... FAIL: got %d tokens, want %d\n", name, n, n_want);
        for (int i = 0; i < n; i++) printf("    got[%d]=\"%s\"\n", i, out[i]);
        exit(1);
    }
    printf("  %s... PASS\n", name);
    free(buf);
}

int main(void) {
    TEST_INIT();
    printf("test_util_split\n");

    expect("null", NULL, 32, NULL, 0);
    expect("empty", "", 32, NULL, 0);
    expect("just_delims", " ,  ,\t, ", 32, NULL, 0);

    {
        const char *w[] = {"a"};
        expect("single", "a", 32, w, 1);
    }
    {
        const char *w[] = {"a", "b", "c"};
        expect("comma_only", "a,b,c", 32, w, 3);
        expect("space_only", "a b c", 32, w, 3);
        expect("comma_space", "a, b, c", 32, w, 3);
        expect("mixed_runs", "  a ,, b\t,  c  ", 32, w, 3);
    }
    {
        /* A hostname/chat-id never has internal whitespace, so this is a
         * deliberate feature, not a false split: a missing comma still
         * separates tokens rather than silently merging them. */
        const char *w[] = {"api.example.com", ".discord.gg"};
        expect("hosts_no_comma", "api.example.com .discord.gg", 32, w, 2);
    }
    {
        /* Cap truncates rather than overruns the output array. */
        const char *w[] = {"a", "b"};
        expect("capped", "a,b,c,d", 2, w, 2);
    }
    {
        const char *w[] = {"x"};
        expect("tabs", "\t x \t", 32, w, 1);
    }

    printf("All tests passed\n");
    return 0;
}
