/* Test secret_interp: deinterpolate forward-only fix + tool_result_postprocess */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "secret_interp.h"
#include "test_util.h"


/* Finding 7: value is substring of own placeholder — must terminate */
static void test_deinterp_self_referential(void) {
    ShellSecret secrets[] = {{"SECRET", "SECRET"}};
    char *r = secret_deinterpolate("found SECRET here", secrets, 1);
    assert(r != NULL);
    assert(strcmp(r, "found {{SECRET:SECRET}} here") == 0);
    free(r);
}

/* Finding 7: value containing {{ — must not infinite loop */
static void test_deinterp_value_contains_braces(void) {
    ShellSecret secrets[] = {{"X", "{{val}}"}};
    char *r = secret_deinterpolate("see {{val}} here", secrets, 1);
    assert(r != NULL);
    assert(strcmp(r, "see {{SECRET:X}} here") == 0);
    free(r);
}

/* Multiple occurrences replaced in one pass */
static void test_deinterp_multiple_occurrences(void) {
    ShellSecret secrets[] = {{"TOK", "abc"}};
    char *r = secret_deinterpolate("abc and abc and abc", secrets, 1);
    assert(r != NULL);
    assert(strcmp(r, "{{SECRET:TOK}} and {{SECRET:TOK}} and {{SECRET:TOK}}") == 0);
    free(r);
}

/* Empty value skipped (no crash on strstr empty) */
static void test_deinterp_empty_value(void) {
    ShellSecret secrets[] = {{"E", ""}};
    char *r = secret_deinterpolate("unchanged text", secrets, 1);
    assert(r != NULL);
    assert(strcmp(r, "unchanged text") == 0);
    free(r);
}

/* Normal case */
static void test_deinterp_normal(void) {
    ShellSecret secrets[] = {{"GH", "ghp_abc123def456"}};
    char *r = secret_deinterpolate("token is ghp_abc123def456 ok", secrets, 1);
    assert(r != NULL);
    assert(strcmp(r, "token is {{SECRET:GH}} ok") == 0);
    free(r);
}

/* Longest-first ordering: longer secret replaced before shorter */
static void test_deinterp_longest_first(void) {
    ShellSecret secrets[] = {{"SHORT", "abc"}, {"LONG", "abcdef"}};
    char *r = secret_deinterpolate("got abcdef here", secrets, 2);
    assert(r != NULL);
    /* "abcdef" should match LONG (longest first), not be split */
    assert(strstr(r, "{{SECRET:LONG}}") != NULL);
    free(r);
}

/* tool_result_postprocess: deinterpolates secret values */
static void test_postprocess_deinterpolates(void) {
    ShellSecret secrets[] = {{"KEY", "s3cr3t"}};
    char *r = tool_result_postprocess("the value is s3cr3t", secrets, 1);
    assert(r != NULL);
    assert(strstr(r, "s3cr3t") == NULL);
    assert(strstr(r, "{{SECRET:KEY}}") != NULL);
    free(r);
}

/* tool_result_postprocess: returns NULL when nothing to do */
static void test_postprocess_noop(void) {
    ShellSecret secrets[] = {{"X", "notpresent"}};
    char *r = tool_result_postprocess("clean output", secrets, 1);
    assert(r == NULL);
}

/* tool_result_postprocess: AC scan must run even with zero secrets loaded
 * (regression: the inline tool path used to skip the scan entirely) */
static void test_postprocess_scans_without_secrets(void) {
    char *r = tool_result_postprocess(
        "key AKIAIOSFODNN7EXAMPLE leaked here", NULL, 0);
    assert(r != NULL);
    assert(strstr(r, "AKIAIOSFODNN7EXAMPLE") == NULL);
    assert(strstr(r, "[SECRET_DETECTED:") != NULL);
    /* expanding tag must not drop the trailing text (headroom is allocated) */
    assert(strstr(r, "leaked here") != NULL);
    free(r);
}

/* interpolate + deinterpolate roundtrip */
static void test_interpolate_roundtrip(void) {
    ShellSecret secrets[] = {{"TOK", "myvalue123"}};
    const char *input = "use {{SECRET:TOK}} here";
    char *interpolated = secret_interpolate(input, secrets, 1);
    assert(interpolated != NULL);
    assert(strcmp(interpolated, "use myvalue123 here") == 0);
    char *back = secret_deinterpolate(interpolated, secrets, 1);
    assert(back != NULL);
    assert(strcmp(back, input) == 0);
    free(interpolated);
    free(back);
}

static void test_placeholder_names_basic(void) {
    size_t n = 0;
    char **names = secret_placeholder_names(
        "curl -H 'Authorization: Bearer {{SECRET:GH_TOKEN}}' "
        "-u user:{{SECRET:NPM_TOKEN}} https://x", &n);
    assert(n == 2 && names);
    assert(strcmp(names[0], "GH_TOKEN") == 0);
    assert(strcmp(names[1], "NPM_TOKEN") == 0);
    secret_names_free(names, n);
}

static void test_placeholder_names_dedup_and_none(void) {
    size_t n = 99;
    char **names = secret_placeholder_names(
        "{{SECRET:A}} then {{SECRET:A}} again", &n);
    assert(n == 1 && names && strcmp(names[0], "A") == 0);
    secret_names_free(names, n);
    names = secret_placeholder_names("no placeholders here", &n);
    assert(n == 0 && names == NULL);
    /* Unterminated placeholder: parse stops, nothing invented */
    names = secret_placeholder_names("{{SECRET:BROKEN", &n);
    assert(n == 0 && names == NULL);
    /* Empty name skipped, later valid one still found */
    names = secret_placeholder_names("{{SECRET:}} {{SECRET:B}}", &n);
    assert(n == 1 && names && strcmp(names[0], "B") == 0);
    secret_names_free(names, n);
}

int main(void) {
    TEST_INIT();
    alarm(5);  /* Hard kill if anything infinite-loops */
    RUN(test_deinterp_self_referential);
    RUN(test_deinterp_value_contains_braces);
    RUN(test_deinterp_multiple_occurrences);
    RUN(test_deinterp_empty_value);
    RUN(test_deinterp_normal);
    RUN(test_deinterp_longest_first);
    RUN(test_postprocess_deinterpolates);
    RUN(test_postprocess_noop);
    RUN(test_postprocess_scans_without_secrets);
    RUN(test_interpolate_roundtrip);
    RUN(test_placeholder_names_basic);
    RUN(test_placeholder_names_dedup_and_none);
    return 0;
}
