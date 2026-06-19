#define _GNU_SOURCE
#include "tool_shell.h"
#include "secret_interp.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Injection still works (env reaches the child), and the raw value never
 * survives the parent masking chokepoint — it becomes {{SECRET:MYKEY}}. */
static void test_shell_inject_then_mask(void) {
    setenv("CCLAW_SECRET_MYKEY", "hunter2xyz", 1);
    size_t count = 0;
    ShellSecret *secrets = shell_secrets_collect(&count);
    assert(count == 1);

    ShellConfig sc = {
        .timeout = 5,
        .workspace = NULL,
        .secrets = secrets,
        .secret_count = count,
        .sandbox = 1,
    };

    /* Handler returns RAW output now (masking moved to the parent) */
    char *result = tool_shell_handler("{\"command\":\"echo $CCLAW_SECRET_MYKEY\"}", &sc);
    assert(result != NULL);
    int echoed = (strstr(result, "hunter2xyz") != NULL); /* sandbox ran the echo */

    /* Parent chokepoint masks it */
    char *pp = tool_result_postprocess(result, secrets, count);
    const char *final = pp ? pp : result;
    assert(strstr(final, "hunter2xyz") == NULL);            /* raw never survives */
    if (echoed) assert(strstr(final, "{{SECRET:MYKEY}}") != NULL);

    free(pp);
    free(result);
    shell_secrets_free(secrets, count);
}

/* Env injection is scoped: a secret the command does not reference via
 * $CCLAW_SECRET_<NAME> must NOT be present in the child env. */
static void test_env_injection_scoped(void) {
    setenv("CCLAW_SECRET_USEDKEY", "value_used_123", 1);
    setenv("CCLAW_SECRET_UNUSEDKEY", "value_unused_456", 1);
    size_t count = 0;
    ShellSecret *secrets = shell_secrets_collect(&count);
    assert(count == 2);

    ShellConfig sc = {
        .timeout = 5, .workspace = NULL,
        .secrets = secrets, .secret_count = count, .sandbox = 1,
    };

    /* References USEDKEY only; never names UNUSEDKEY. Dump injected secret var
     * NAMES (values stripped) so we observe which were injected without the
     * command ever referencing UNUSEDKEY. */
    char *result = tool_shell_handler(
        "{\"command\":\"echo $CCLAW_SECRET_USEDKEY >/dev/null; "
        "env | grep '^CCLAW_SECRET_' | sed 's/=.*//'\"}",
        &sc);
    assert(result != NULL);
    if (strstr(result, "namespace sandbox unavailable") == NULL) {
        assert(strstr(result, "CCLAW_SECRET_USEDKEY") != NULL);    /* referenced → injected */
        assert(strstr(result, "CCLAW_SECRET_UNUSEDKEY") == NULL);  /* unreferenced → absent */
    }
    free(result);
    shell_secrets_free(secrets, count);
}

int main(void) {
    alarm(10);
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
    test_shell_inject_then_mask();
    test_env_injection_scoped();
    printf("test_shell_secrets: all tests passed\n");
    return 0;
}
