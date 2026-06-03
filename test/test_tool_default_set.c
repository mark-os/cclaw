/* T275/V119: default tool set enforcement when CCLAW_TOOLS absent */
#define _POSIX_C_SOURCE 200809L
#include "tools.h"
#include "agent_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* V119/V124: default tools from agent_config.h */
static const char *EXPECTED_DEFAULTS[] = { AGENT_DEFAULT_TOOLS };
static const size_t EXPECTED_COUNT = AGENT_DEFAULT_TOOLS_COUNT;

static char *dummy_handler(const char *args, void *ud) {
    (void)args; (void)ud;
    return strdup("ok");
}

/* When whitelist = DEFAULT_TOOLS, only those 7 tools are exposed */
static void test_default_whitelist_filters(void) {
    ToolRegistry reg;
    tools_init(&reg);

    /* Register more tools than the default set */
    tools_register(&reg, "file_read", "read", "{}", dummy_handler, NULL);
    tools_register(&reg, "file_write", "write", "{}", dummy_handler, NULL);
    tools_register(&reg, "js_eval", "eval", "{}", dummy_handler, NULL);
    tools_register(&reg, "memory_create", "mem create", "{}", dummy_handler, NULL);
    tools_register(&reg, "memory_append", "mem append", "{}", dummy_handler, NULL);
    tools_register(&reg, "memory_replace", "mem replace", "{}", dummy_handler, NULL);
    tools_register(&reg, "request_config", "config", "{}", dummy_handler, NULL);
    tools_register(&reg, "shell_exec", "shell", "{}", dummy_handler, NULL);
    tools_register(&reg, "web_fetch", "fetch", "{}", dummy_handler, NULL);
    tools_register(&reg, "db_query", "query", "{}", dummy_handler, NULL);

    /* Apply default whitelist */
    size_t count = 0;
    const ToolSchema *s = tools_schemas_filtered(&reg, EXPECTED_DEFAULTS,
                                                  EXPECTED_COUNT, &count);
    assert(count == EXPECTED_COUNT);

    /* Verify only default tools present */
    for (size_t i = 0; i < count; i++) {
        int found = 0;
        for (size_t j = 0; j < EXPECTED_COUNT; j++) {
            if (strcmp(s[i].name, EXPECTED_DEFAULTS[j]) == 0) {
                found = 1;
                break;
            }
        }
        assert(found);
    }

    /* Verify non-default tools excluded */
    assert(!tools_is_whitelisted("shell_exec", EXPECTED_DEFAULTS, EXPECTED_COUNT));
    assert(!tools_is_whitelisted("web_fetch", EXPECTED_DEFAULTS, EXPECTED_COUNT));
    assert(!tools_is_whitelisted("db_query", EXPECTED_DEFAULTS, EXPECTED_COUNT));

    tools_free(&reg);
}

/* Verify all 7 default tools are individually whitelisted */
static void test_all_defaults_whitelisted(void) {
    for (size_t i = 0; i < EXPECTED_COUNT; i++) {
        assert(tools_is_whitelisted(EXPECTED_DEFAULTS[i],
                                    EXPECTED_DEFAULTS, EXPECTED_COUNT));
    }
}

int main(void) {
    test_default_whitelist_filters();
    test_all_defaults_whitelisted();
    printf("test_tool_default_set: all passed (V119)\n");
    return 0;
}
