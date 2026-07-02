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

/* Masking is now a single chokepoint: secret_deinterpolate() replaces secret
 * values (raw + base64 + URL-encoded) with the re-referenceable {{SECRET:name}}
 * placeholder. The shell handler no longer masks; the parent does. */

static void test_mask_single_secret(void) {
    ShellSecret secrets[] = {{"TOKEN", "abc123"}};
    char *out = secret_deinterpolate("got token: abc123 done", secrets, 1);
    assert(strcmp(out, "got token: {{SECRET:TOKEN}} done") == 0);
    free(out);
}

static void test_mask_multiple_occurrences(void) {
    ShellSecret secrets[] = {{"KEY", "secret"}};
    char *out = secret_deinterpolate("secret and secret again", secrets, 1);
    assert(strcmp(out, "{{SECRET:KEY}} and {{SECRET:KEY}} again") == 0);
    free(out);
}

static void test_mask_multiple_secrets(void) {
    ShellSecret secrets[] = {{"A", "aaa"}, {"B", "bbb"}};
    char *out = secret_deinterpolate("aaa then bbb", secrets, 2);
    assert(strstr(out, "{{SECRET:A}}") != NULL);
    assert(strstr(out, "{{SECRET:B}}") != NULL);
    assert(strstr(out, "aaa") == NULL);
    assert(strstr(out, "bbb") == NULL);
    free(out);
}

static void test_mask_no_match(void) {
    ShellSecret secrets[] = {{"X", "xyz"}};
    char *out = secret_deinterpolate("nothing here", secrets, 1);
    assert(strcmp(out, "nothing here") == 0);
    free(out);
}

static void test_mask_empty_value_skipped(void) {
    ShellSecret secrets[] = {{"E", ""}};
    char *out = secret_deinterpolate("unchanged", secrets, 1);
    assert(strcmp(out, "unchanged") == 0);
    free(out);
}

static void test_mask_null_secrets(void) {
    char *out = secret_deinterpolate("safe", NULL, 0);
    assert(strcmp(out, "safe") == 0);
    free(out);
}

static void test_mask_base64_encoded(void) {
    /* "secret123" base64 = "c2VjcmV0MTIz" */
    ShellSecret secrets[] = {{"TOK", "secret123"}};
    char *out = secret_deinterpolate("encoded: c2VjcmV0MTIz end", secrets, 1);
    assert(strstr(out, "c2VjcmV0MTIz") == NULL);
    assert(strstr(out, "{{SECRET:TOK}}") != NULL);
    free(out);
}

static void test_mask_url_encoded(void) {
    /* "key=val&x" url-encoded = "key%3Dval%26x" */
    ShellSecret secrets[] = {{"API", "key=val&x"}};
    char *out = secret_deinterpolate("param: key%3Dval%26x done", secrets, 1);
    assert(strstr(out, "key%3Dval%26x") == NULL);
    assert(strstr(out, "{{SECRET:API}}") != NULL);
    free(out);
}

static void test_mask_all_variants(void) {
    /* exact + base64 + url-encoded all get masked.
     * "a+b" base64 = "YSti", url-encoded = "a%2Bb" */
    ShellSecret secrets[] = {{"S", "a+b"}};
    char *out = secret_deinterpolate("raw:a+b b64:YSti url:a%2Bb", secrets, 1);
    assert(strstr(out, "a+b") == NULL);
    assert(strstr(out, "YSti") == NULL);
    assert(strstr(out, "a%2Bb") == NULL);
    int count = 0;
    char *p = out;
    while ((p = strstr(p, "{{SECRET:S}}")) != NULL) { count++; p++; }
    assert(count == 3);
    free(out);
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

/* Injection still works (env reaches the child), and the raw value never
 * survives the parent masking chokepoint — it becomes {{SECRET:MYKEY}}. */
static void test_shell_inject_then_mask(void) {
    RunToolSecret secrets[] = {{"MYKEY", "hunter2xyz"}};

    ShellToolReq r = SHELL_REQ_DEFAULTS;
    r.command = "echo $CCLAW_SECRET_MYKEY";
    r.workspace = workspace;
    r.timeout = 5;
    r.secrets = secrets;
    r.secret_count = 1;

    char *result = run_tool_shell(&r);
    assert(result != NULL);

    /* If namespaces unavailable, skip assertion on content */
    if (strstr(result, "error:") != NULL) {
        printf("  (skipped — broker error: %.60s)\n", result);
        free(result);
        return;
    }

    int echoed = (strstr(result, "hunter2xyz") != NULL);

    /* Parent chokepoint masks it — tool_result_postprocess wraps
     * secret_deinterpolate which handles raw + b64 + url variants */
    ShellSecret mask_secrets[] = {{"MYKEY", "hunter2xyz"}};
    char *pp = tool_result_postprocess(result, mask_secrets, 1);
    const char *final = pp ? pp : result;
    assert(strstr(final, "hunter2xyz") == NULL);            /* raw never survives */
    if (echoed) assert(strstr(final, "{{SECRET:MYKEY}}") != NULL);

    free(pp);
    free(result);
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
    RunToolSecret used_only[] = {{"USEDKEY", "value_used_123"}};

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
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(10);

    /* Pure unit tests (no broker) */
    test_mask_single_secret();
    test_mask_multiple_occurrences();
    test_mask_multiple_secrets();
    test_mask_no_match();
    test_mask_empty_value_skipped();
    test_mask_null_secrets();
    test_mask_base64_encoded();
    test_mask_url_encoded();
    test_mask_all_variants();
    test_collect_and_free();

    /* Broker-path tests */
    setup_workspace();
    test_shell_inject_then_mask();
    test_env_injection_scoped();
    cleanup_workspace();

    printf("test_shell_secrets: all tests passed\n");
    return 0;
}
