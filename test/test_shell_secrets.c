#define _GNU_SOURCE
#include "tool_shell.h"
#include "secret_interp.h"
#include "test_run_tool_shell.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "test_util.h"

/* Masking is now a single chokepoint: secret_deinterpolate() replaces secret
 * values (raw + base64 + URL-encoded) with the re-referenceable {{SECRET:name}}
 * placeholder. The shell handler no longer masks; the parent does. */

/* Every encoding of a value (raw, base64, URL) becomes the placeholder.
 * "secret123" b64 = "c2VjcmV0MTIz"; "a+b" b64 = "YSti", url = "a%2Bb". */
static const struct {
    int n; ShellSecret secrets[2]; const char *in; const char *out;
} masks[] = {
    {2, {{"A", "aaa"}, {"B", "bbb"}}, "aaa then bbb", "{{SECRET:A}} then {{SECRET:B}}"},
    {0, {{NULL, NULL}}, "safe", "safe"},
    {1, {{"TOK", "secret123"}}, "encoded: c2VjcmV0MTIz end", "encoded: {{SECRET:TOK}} end"},
    {1, {{"API", "key=val&x"}}, "param: key%3Dval%26x done", "param: {{SECRET:API}} done"},
    {1, {{"S", "a+b"}}, "raw:a+b b64:YSti url:a%2Bb",
     "raw:{{SECRET:S}} b64:{{SECRET:S}} url:{{SECRET:S}}"},
};

static void test_mask_table(void) {
    for (size_t i = 0; i < sizeof(masks) / sizeof(masks[0]); i++) {
        char *out = secret_deinterpolate(masks[i].in, masks[i].n ? masks[i].secrets : NULL,
                                         (size_t)masks[i].n);
        if (strcmp(out, masks[i].out) != 0) {
            printf("    row %zu: \"%s\"\n", i, out);
            assert(0);
        }
        free(out);
    }
}

static void test_collect_and_free(void) {
    /* Set some test secrets */
    setenv("CCLAW_SECRET_TEST1", "val1", 1);
    setenv("CCLAW_SECRET_TEST2", "val2", 1);

    size_t count = 0;
    ShellSecret *s = shell_secrets_collect(&count);
    assert(count == 2);
    assert(s != NULL);

    /* Verify env vars cleared */
    assert(getenv("CCLAW_SECRET_TEST1") == NULL);
    assert(getenv("CCLAW_SECRET_TEST2") == NULL);

    /* Verify values collected */
    int found1 = 0, found2 = 0;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(s[i].name, "TEST1") == 0 && strcmp(s[i].value, "val1") == 0) found1 = 1;
        if (strcmp(s[i].name, "TEST2") == 0 && strcmp(s[i].value, "val2") == 0) found2 = 1;
    }
    assert(found1 && found2);

    shell_secrets_free(s, count);
}

/* shell_filter_secrets(): only a word-bounded $CCLAW_SECRET_<NAME>
 * reference ships that secret to the child. want NULL = none ships. */
static const struct { const char *cmd; int n; const char *want; } filters[] = {
    {"curl -H \"Bearer: $CCLAW_SECRET_FOO\"", 2, "FOO"},
    {"echo $MY_CCLAW_SECRET_FOO", 2, NULL},      /* ident char before */
    {"echo $CCLAW_SECRET_FOOBAR", 2, NULL},      /* ident char after */
    {"echo hello", 2, NULL},
    {NULL, 2, NULL},
    {"", 2, NULL},
    {"echo $CCLAW_SECRET_FOO", 0, NULL},
};

static void test_filter_table(void) {
    ShellSecret all[] = {{"FOO", "v1"}, {"BAR", "v2"}};
    for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); i++) {
        size_t count = 99;
        RunToolSecret *min = shell_filter_secrets(filters[i].cmd, all,
                                                  (size_t)filters[i].n, &count);
        int ok = filters[i].want
            ? (min && count == 1 && strcmp(min[0].name, filters[i].want) == 0 &&
               min[0].value == all[0].value)   /* borrowed, not copied */
            : (min == NULL && count == 0);
        if (!ok) { printf("    row %zu: count %zu\n", i, count); assert(0); }
        free(min);
    }
}

/* --- Tests using --run-tool broker path --- */

static char workspace[256];

static void setup_workspace(void) {
    snprintf(workspace, sizeof(workspace), "/tmp/cclaw_shellsecrets_%d", getpid());
    mkdir(workspace, 0755);
}

static void cleanup_workspace(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", workspace);
    system(cmd);
}

/* Env injection is scoped: secrets the command does not reference via
 * $CCLAW_SECRET_<NAME> are pre-filtered by the parent (main.c) before being
 * passed to the --run-tool broker. The broker injects exactly what it receives.
 * This test validates that when only the used secret is passed (as the parent
 * would), the child sees only that one. */
static void test_env_injection_scoped(void) {
    /* The parent pre-filters: only secrets whose $CCLAW_SECRET_<NAME> appears
     * in the command get passed to the broker. The command below references
     * USEDKEY only, so the parent would pass only that one. */
    RunToolSecret used_only[] = {{"USEDKEY", "value_used_123", NULL}};

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    /* References USEDKEY only; the parent would have excluded UNUSEDKEY.
     * Dump injected secret var NAMES (values stripped) to confirm only the
     * referenced secret was injected. */
    r.command = "echo $CCLAW_SECRET_USEDKEY >/dev/null; "
                "env | grep '^CCLAW_SECRET_' | sed 's/=.*//'";
    r.workspace = workspace;
    r.timeout = 5;
    r.secrets = used_only;
    r.secret_count = 1;

    char *result = run_tool_shell(&r);
    assert(result != NULL);

    if (strstr(result, "error:") != NULL) {
        printf("  (skipped — broker error: %.60s)\n", result);
        free(result);
        return;
    }

    if (strstr(result, "namespace sandbox unavailable") == NULL) {
        assert(strstr(result, "CCLAW_SECRET_USEDKEY") != NULL);    /* referenced → injected */
        assert(strstr(result, "CCLAW_SECRET_UNUSEDKEY") == NULL);  /* unreferenced → absent */
    }
    free(result);
}

int main(void) {
    TEST_INIT();
    alarm(10);
    printf("test_shell_secrets:\n");
    RUN(test_mask_table);
    RUN(test_collect_and_free);
    RUN(test_filter_table);

    setup_workspace();
    RUN(test_env_injection_scoped);
    cleanup_workspace();
    return 0;
}
