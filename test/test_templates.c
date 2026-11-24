/* Test: templates.h generated correctly */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "templates.h"
#include "test_util.h"

int main(void) {
    TEST_INIT();
    assert(strstr(TPL_SCHEMA_SQL, "CREATE TABLE IF NOT EXISTS agents"));
    assert(strstr(TPL_SCHEMA_SQL, "CREATE TABLE IF NOT EXISTS entries"));
    /* Default system prompt has template vars */
    assert(strstr(TPL_DEFAULT_SYSTEM_PROMPT_MD, "{workspace}"));
    printf("test_templates: all tests passed\n");
    return 0;
}
