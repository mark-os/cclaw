/* Test: templates.h generated correctly */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "templates.h"

int main(void) {
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

    /* Notices are non-empty */
    assert(strlen(TPL_CUTOFF_NOTICE_TXT) > 10);
    assert(strlen(TPL_INCOMPLETE_TURN_NOTICE_TXT) > 10);
    assert(strlen(TPL_INCOMPLETE_TOOL_CONTENT_TXT) > 5);
    printf("  notices_non_empty... PASS\n");

    /* Skill shell template exists */
    assert(strstr(TPL_SKILL_SHELL_MD, "shell_exec"));
    printf("  skill_shell_content... PASS\n");

    printf("4/4 passed\n");
    return 0;
}
