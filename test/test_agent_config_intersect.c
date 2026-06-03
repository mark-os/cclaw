/* T279/V123: Unit test for agent_config_intersect — sub-agent privilege reduction */
#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static AgentConfig *make_config(const char **tools, size_t tc,
                                const char **hosts, size_t hc, int max_iter) {
    AgentConfig *ac = calloc(1, sizeof(AgentConfig));
    if (tc > 0) {
        ac->tools = malloc(tc * sizeof(char *));
        for (size_t i = 0; i < tc; i++) ac->tools[i] = strdup(tools[i]);
        ac->tool_count = tc;
    }
    if (hc > 0) {
        ac->allowed_hosts = malloc(hc * sizeof(char *));
        for (size_t i = 0; i < hc; i++) ac->allowed_hosts[i] = strdup(hosts[i]);
        ac->allowed_hosts_count = hc;
    }
    ac->max_iterations = max_iter;
    return ac;
}

static int has_tool(AgentConfig *ac, const char *name) {
    for (size_t i = 0; i < ac->tool_count; i++)
        if (strcmp(ac->tools[i], name) == 0) return 1;
    return 0;
}

static int has_host(AgentConfig *ac, const char *name) {
    for (size_t i = 0; i < ac->allowed_hosts_count; i++)
        if (strcmp(ac->allowed_hosts[i], name) == 0) return 1;
    return 0;
}

static void test_tools_intersection(void) {
    const char *parent_tools[] = {"file_read", "file_write", "js_eval"};
    const char *child_tools[] = {"file_read", "shell_exec", "js_eval"};
    AgentConfig *parent = make_config(parent_tools, 3, NULL, 0, 0);
    AgentConfig *child = make_config(child_tools, 3, NULL, 0, 0);

    agent_config_intersect(child, parent);

    assert(child->tool_count == 2);
    assert(has_tool(child, "file_read"));
    assert(has_tool(child, "js_eval"));
    assert(!has_tool(child, "shell_exec"));

    agent_config_free(child);
    agent_config_free(parent);
    printf("  PASS test_tools_intersection\n");
}

static void test_hosts_intersection(void) {
    const char *parent_hosts[] = {"api.openai.com", "example.com"};
    const char *child_hosts[] = {"example.com", "evil.com", "api.openai.com"};
    AgentConfig *parent = make_config(NULL, 0, parent_hosts, 2, 0);
    AgentConfig *child = make_config(NULL, 0, child_hosts, 3, 0);

    agent_config_intersect(child, parent);

    assert(child->allowed_hosts_count == 2);
    assert(has_host(child, "example.com"));
    assert(has_host(child, "api.openai.com"));
    assert(!has_host(child, "evil.com"));

    agent_config_free(child);
    agent_config_free(parent);
    printf("  PASS test_hosts_intersection\n");
}

static void test_max_iterations_min(void) {
    AgentConfig *parent = make_config(NULL, 0, NULL, 0, 10);
    AgentConfig *child = make_config(NULL, 0, NULL, 0, 25);

    agent_config_intersect(child, parent);
    assert(child->max_iterations == 10);

    agent_config_free(child);
    agent_config_free(parent);
    printf("  PASS test_max_iterations_min\n");
}

static void test_child_inherits_parent_when_empty(void) {
    const char *parent_tools[] = {"file_read", "js_eval"};
    const char *parent_hosts[] = {"api.example.com"};
    AgentConfig *parent = make_config(parent_tools, 2, parent_hosts, 1, 15);
    AgentConfig *child = make_config(NULL, 0, NULL, 0, 0);

    agent_config_intersect(child, parent);

    /* Child adopts parent's tools/hosts/iterations as ceiling */
    assert(child->tool_count == 2);
    assert(has_tool(child, "file_read"));
    assert(has_tool(child, "js_eval"));
    assert(child->allowed_hosts_count == 1);
    assert(has_host(child, "api.example.com"));
    assert(child->max_iterations == 15);

    agent_config_free(child);
    agent_config_free(parent);
    printf("  PASS test_child_inherits_parent_when_empty\n");
}

static void test_child_lower_iterations_kept(void) {
    AgentConfig *parent = make_config(NULL, 0, NULL, 0, 25);
    AgentConfig *child = make_config(NULL, 0, NULL, 0, 5);

    agent_config_intersect(child, parent);
    assert(child->max_iterations == 5);

    agent_config_free(child);
    agent_config_free(parent);
    printf("  PASS test_child_lower_iterations_kept\n");
}

static void test_null_parent_noop(void) {
    const char *child_tools[] = {"shell_exec"};
    AgentConfig *child = make_config(child_tools, 1, NULL, 0, 30);

    agent_config_intersect(child, NULL);

    assert(child->tool_count == 1);
    assert(has_tool(child, "shell_exec"));
    assert(child->max_iterations == 30);

    agent_config_free(child);
    printf("  PASS test_null_parent_noop\n");
}

int main(void) {
    printf("test_agent_config_intersect (T279/V123):\n");
    test_tools_intersection();
    test_hosts_intersection();
    test_max_iterations_min();
    test_child_inherits_parent_when_empty();
    test_child_lower_iterations_kept();
    test_null_parent_noop();
    printf("All agent_config_intersect tests passed.\n");
    return 0;
}
