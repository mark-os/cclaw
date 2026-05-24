#include "agent_config.h"
#include "config.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void make_dir(const char *path) {
    mkdir(path, 0755);
}

static int has_name(char **names, size_t count, const char *name) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) return 1;
    }
    return 0;
}

static void test_discover_agents(void) {
    /* Setup temp agents dir */
    const char *base = "/tmp/test_agent_discover";
    system("rm -rf /tmp/test_agent_discover");
    make_dir(base);
    make_dir("/tmp/test_agent_discover/coder");
    make_dir("/tmp/test_agent_discover/researcher");
    make_dir("/tmp/test_agent_discover/planner");

    /* Create a file (should be ignored — not a dir) */
    FILE *f = fopen("/tmp/test_agent_discover/README.md", "w");
    if (f) { fprintf(f, "ignore me"); fclose(f); }

    size_t count = 0;
    char **names = agent_discover(base, &count);
    assert(names != NULL);
    assert(count == 3);
    assert(has_name(names, count, "coder"));
    assert(has_name(names, count, "researcher"));
    assert(has_name(names, count, "planner"));
    agent_discover_free(names, count);

    system("rm -rf /tmp/test_agent_discover");
    printf("  PASS: discover agents\n");
}

static void test_discover_empty(void) {
    const char *base = "/tmp/test_agent_discover_empty";
    system("rm -rf /tmp/test_agent_discover_empty");
    make_dir(base);

    size_t count = 99;
    char **names = agent_discover(base, &count);
    assert(names != NULL);
    assert(count == 0);
    agent_discover_free(names, count);

    system("rm -rf /tmp/test_agent_discover_empty");
    printf("  PASS: discover empty dir\n");
}

static void test_discover_missing_dir(void) {
    size_t count = 99;
    char **names = agent_discover("/tmp/nonexistent_agents_xyz", &count);
    assert(names == NULL);
    assert(count == 0);
    printf("  PASS: discover missing dir\n");
}

/* T76 tests */

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void test_agent_config_load(void) {
    system("rm -rf /tmp/test_ac_load");
    make_dir("/tmp/test_ac_load");
    make_dir("/tmp/test_ac_load/coder");

    write_file("/tmp/test_ac_load/coder/agent.json",
        "{"
        "\"model\": \"anthropic/claude-sonnet\","
        "\"workspace\": \"/home/coder/work\","
        "\"max_iterations\": 50,"
        "\"tools\": [\"shell_exec\", \"file_read\", \"file_write\"],"
        "\"allowed_hosts\": [\"api.github.com\", \"example.com\"]"
        "}");

    AgentConfig *ac = agent_config_load("/tmp/test_ac_load", "coder");
    assert(ac != NULL);
    assert(strcmp(ac->name, "coder") == 0);
    assert(strcmp(ac->model, "anthropic/claude-sonnet") == 0);
    assert(strcmp(ac->workspace, "/home/coder/work") == 0);
    assert(ac->max_iterations == 50);
    assert(ac->tool_count == 3);
    assert(strcmp(ac->tools[0], "shell_exec") == 0);
    assert(strcmp(ac->tools[2], "file_write") == 0);
    assert(ac->allowed_hosts_count == 2);
    assert(strcmp(ac->allowed_hosts[0], "api.github.com") == 0);
    agent_config_free(ac);

    system("rm -rf /tmp/test_ac_load");
    printf("  PASS: agent config load\n");
}

static void test_agent_config_workspace_fallback(void) {
    /* V12: no workspace in JSON → fallback to ./workspace/{name} */
    system("rm -rf /tmp/test_ac_ws");
    make_dir("/tmp/test_ac_ws");
    make_dir("/tmp/test_ac_ws/researcher");
    write_file("/tmp/test_ac_ws/researcher/agent.json", "{\"max_iterations\": 10}");

    AgentConfig *ac = agent_config_load("/tmp/test_ac_ws", "researcher");
    assert(ac != NULL);
    assert(strcmp(ac->workspace, "./workspace/researcher") == 0);
    assert(ac->model == NULL);
    assert(ac->max_iterations == 10);
    agent_config_free(ac);

    system("rm -rf /tmp/test_ac_ws");
    printf("  PASS: agent config workspace fallback (V12)\n");
}

static void test_agent_config_missing_file(void) {
    /* No agent.json → returns NULL */
    system("rm -rf /tmp/test_ac_miss");
    make_dir("/tmp/test_ac_miss");
    make_dir("/tmp/test_ac_miss/ghost");

    AgentConfig *ac = agent_config_load("/tmp/test_ac_miss", "ghost");
    assert(ac == NULL);

    system("rm -rf /tmp/test_ac_miss");
    printf("  PASS: agent config missing file\n");
}

static void test_agent_config_merge(void) {
    /* V20: merge agent overrides into global config */
    Config *global = config_load(NULL);
    assert(global != NULL);

    system("rm -rf /tmp/test_ac_merge");
    make_dir("/tmp/test_ac_merge");
    make_dir("/tmp/test_ac_merge/bot");
    write_file("/tmp/test_ac_merge/bot/agent.json",
        "{\"model\": \"openai/gpt-4o\", \"max_iterations\": 100}");

    AgentConfig *ac = agent_config_load("/tmp/test_ac_merge", "bot");
    assert(ac != NULL);

    Config *merged = agent_config_merge(global, ac);
    assert(merged != NULL);
    /* Model overridden */
    assert(strcmp(merged->provider.model, "openai/gpt-4o") == 0);
    /* Workspace from agent (V12 fallback) */
    assert(strcmp(merged->workspace, "./workspace/bot") == 0);
    /* max_iterations overridden */
    assert(merged->max_iterations == 100);
    /* base_url preserved from global */
    assert(merged->provider.base_url != NULL);
    assert(strcmp(merged->provider.base_url, global->provider.base_url) == 0);

    config_free(merged);
    agent_config_free(ac);
    config_free(global);
    system("rm -rf /tmp/test_ac_merge");
    printf("  PASS: agent config merge (V20)\n");
}

static void test_agent_config_merge_null(void) {
    /* NULL agent config → returns copy of global */
    Config *global = config_load(NULL);
    assert(global != NULL);

    Config *merged = agent_config_merge(global, NULL);
    assert(merged != NULL);
    assert(strcmp(merged->provider.model, global->provider.model) == 0);
    assert(strcmp(merged->workspace, global->workspace) == 0);
    assert(merged->max_iterations == global->max_iterations);

    config_free(merged);
    config_free(global);
    printf("  PASS: agent config merge NULL (global copy)\n");
}

/* T77 tests */

static void test_agent_load_system_prompt(void) {
    system("rm -rf /tmp/test_ac_sysprompt");
    make_dir("/tmp/test_ac_sysprompt");
    make_dir("/tmp/test_ac_sysprompt/coder");

    write_file("/tmp/test_ac_sysprompt/coder/system.md",
        "You are {agent_name}. Session: {session_id}. Date: {date}.");

    char *prompt = agent_load_system_prompt("/tmp/test_ac_sysprompt", "coder", 42);
    assert(prompt != NULL);
    /* Check agent_name replaced */
    assert(strstr(prompt, "You are coder.") != NULL);
    /* Check session_id replaced */
    assert(strstr(prompt, "Session: 42.") != NULL);
    /* Check {date} replaced (should be YYYY-MM-DD format) */
    assert(strstr(prompt, "{date}") == NULL);
    assert(strstr(prompt, "Date: 20") != NULL); /* starts with 20xx */
    free(prompt);

    system("rm -rf /tmp/test_ac_sysprompt");
    printf("  PASS: agent load system prompt (T77)\n");
}

static void test_agent_load_system_prompt_missing(void) {
    /* No system.md → returns NULL */
    system("rm -rf /tmp/test_ac_sysprompt2");
    make_dir("/tmp/test_ac_sysprompt2");
    make_dir("/tmp/test_ac_sysprompt2/ghost");

    char *prompt = agent_load_system_prompt("/tmp/test_ac_sysprompt2", "ghost", 1);
    assert(prompt == NULL);

    system("rm -rf /tmp/test_ac_sysprompt2");
    printf("  PASS: agent load system prompt missing file\n");
}

int main(void) {
    printf("test_agent_config:\n");
    test_discover_agents();
    test_discover_empty();
    test_discover_missing_dir();
    test_agent_config_load();
    test_agent_config_workspace_fallback();
    test_agent_config_missing_file();
    test_agent_config_merge();
    test_agent_config_merge_null();
    test_agent_load_system_prompt();
    test_agent_load_system_prompt_missing();
    printf("All tests passed.\n");
    return 0;
}
