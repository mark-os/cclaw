#define _GNU_SOURCE
#include "tool_shell.h"
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_mask_single_secret(void) {
    ShellSecret secrets[] = {{"TOKEN", "abc123"}};
    char buf[256];
    strcpy(buf, "got token: abc123 done");
    size_t len = strlen(buf);
    shell_mask_secrets(buf, &len, sizeof(buf), secrets, 1);
    assert(strcmp(buf, "got token: [REDACTED:TOKEN] done") == 0);
    assert(len == strlen(buf));
}

static void test_mask_multiple_occurrences(void) {
    ShellSecret secrets[] = {{"KEY", "secret"}};
    char buf[256];
    strcpy(buf, "secret and secret again");
    size_t len = strlen(buf);
    shell_mask_secrets(buf, &len, sizeof(buf), secrets, 1);
    assert(strcmp(buf, "[REDACTED:KEY] and [REDACTED:KEY] again") == 0);
}

static void test_mask_multiple_secrets(void) {
    ShellSecret secrets[] = {{"A", "aaa"}, {"B", "bbb"}};
    char buf[256];
    strcpy(buf, "aaa then bbb");
    size_t len = strlen(buf);
    shell_mask_secrets(buf, &len, sizeof(buf), secrets, 2);
    assert(strstr(buf, "[REDACTED:A]") != NULL);
    assert(strstr(buf, "[REDACTED:B]") != NULL);
    assert(strstr(buf, "aaa") == NULL);
    assert(strstr(buf, "bbb") == NULL);
}

static void test_mask_no_match(void) {
    ShellSecret secrets[] = {{"X", "xyz"}};
    char buf[256];
    strcpy(buf, "nothing here");
    size_t len = strlen(buf);
    shell_mask_secrets(buf, &len, sizeof(buf), secrets, 1);
    assert(strcmp(buf, "nothing here") == 0);
}

static void test_mask_empty_value_skipped(void) {
    ShellSecret secrets[] = {{"E", ""}};
    char buf[256];
    strcpy(buf, "unchanged");
    size_t len = strlen(buf);
    shell_mask_secrets(buf, &len, sizeof(buf), secrets, 1);
    assert(strcmp(buf, "unchanged") == 0);
}

static void test_mask_null_secrets(void) {
    char buf[64] = "safe";
    size_t len = 4;
    shell_mask_secrets(buf, &len, sizeof(buf), NULL, 0);
    assert(strcmp(buf, "safe") == 0);
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

static void test_shell_injects_secrets(void) {
    /* Set a secret, register shell with it */
    setenv("CCLAW_SECRET_MYKEY", "hunter2", 1);
    size_t count = 0;
    ShellSecret *secrets = shell_secrets_collect(&count);
    assert(count == 1);

    ShellConfig sc = {
        .timeout = 5,
        .workspace = NULL,
        .proxy_sock = NULL,
        .secrets = secrets,
        .secret_count = count,
    };

    /* Run echo $CCLAW_SECRET_MYKEY — child should have it */
    char *result = tool_shell_handler("{\"command\":\"echo $CCLAW_SECRET_MYKEY\"}", &sc);
    assert(result != NULL);
    /* The output should be masked: hunter2 → [REDACTED:MYKEY] */
    assert(strstr(result, "hunter2") == NULL);
    assert(strstr(result, "[REDACTED:MYKEY]") != NULL);
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
    test_collect_and_free();
    test_shell_injects_secrets();
    printf("test_shell_secrets: all tests passed\n");
    return 0;
}
