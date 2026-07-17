/* Test: templates.h generated correctly */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "templates.h"
#include "test_util.h"

int main(void) {
    TEST_INIT();
    printf("test_templates:\n");

    /* unified schema template contains expected tables */
    assert(strstr(TPL_SCHEMA_SQL, "CREATE TABLE IF NOT EXISTS agents"));
    assert(strstr(TPL_SCHEMA_SQL, "CREATE TABLE IF NOT EXISTS config"));
    assert(strstr(TPL_SCHEMA_SQL, "CREATE TABLE IF NOT EXISTS sessions"));
    assert(strstr(TPL_SCHEMA_SQL, "CREATE TABLE IF NOT EXISTS entries"));
    assert(strstr(TPL_SCHEMA_SQL, "CREATE TABLE IF NOT EXISTS tool_calls"));
    printf("  schema_content... PASS\n");

    /* Default system prompt has template vars */
    assert(strstr(TPL_DEFAULT_SYSTEM_PROMPT_MD, "{workspace}"));
    printf("  system_prompt_template_vars... PASS\n");

    printf("2/2 passed\n");
    return 0;
}
