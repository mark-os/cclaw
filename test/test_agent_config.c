#define _POSIX_C_SOURCE 200809L
#include "agent_config.h"
#include "config.h"
#include "db.h"
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
    unlink("/tmp/test_ac_merge.db");
    sqlite3 *db = db_open("/tmp/test_ac_merge.db");
    assert(db);
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    Config *global = config_load(db);
    db_close(db);
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
    unlink("/tmp/test_ac_merge.db");
    printf("  PASS: agent config merge (V20)\n");
}

static void test_agent_config_merge_null(void) {
    /* NULL agent config → returns copy of global */
    unlink("/tmp/test_ac_merge_null.db");
    sqlite3 *db = db_open("/tmp/test_ac_merge_null.db");
    assert(db);
    setenv("OPENROUTER_API_KEY", "sk-test", 1);
    Config *global = config_load(db);
    db_close(db);
    assert(global != NULL);

    Config *merged = agent_config_merge(global, NULL);
    assert(merged != NULL);
    assert(strcmp(merged->provider.model, global->provider.model) == 0);
    assert(strcmp(merged->workspace, global->workspace) == 0);
    assert(merged->max_iterations == global->max_iterations);

    config_free(merged);
    config_free(global);
    unlink("/tmp/test_ac_merge_null.db");
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

/* T80 tests */

static void test_agent_load_skills(void) {
    system("rm -rf /tmp/test_ac_skills");
    make_dir("/tmp/test_ac_skills");
    make_dir("/tmp/test_ac_skills/coder");
    make_dir("/tmp/test_ac_skills/coder/skills");

    write_file("/tmp/test_ac_skills/coder/skills/coding.md", "You write clean code.");
    write_file("/tmp/test_ac_skills/coder/skills/review.md", "You review PRs carefully.");
    /* Non-.md file should be ignored */
    write_file("/tmp/test_ac_skills/coder/skills/notes.txt", "IGNORED");

    char *skills = agent_load_skills("/tmp/test_ac_skills", "coder");
    assert(skills != NULL);
    assert(strstr(skills, "You write clean code.") != NULL);
    assert(strstr(skills, "You review PRs carefully.") != NULL);
    assert(strstr(skills, "IGNORED") == NULL);
    free(skills);

    system("rm -rf /tmp/test_ac_skills");
    printf("  PASS: agent load skills (T80)\n");
}

static void test_agent_load_skills_missing_dir(void) {
    char *skills = agent_load_skills("/tmp/nonexistent_xyz", "ghost");
    assert(skills == NULL);
    printf("  PASS: agent load skills missing dir\n");
}

static void test_agent_load_skills_empty(void) {
    system("rm -rf /tmp/test_ac_skills_empty");
    make_dir("/tmp/test_ac_skills_empty");
    make_dir("/tmp/test_ac_skills_empty/bot");
    make_dir("/tmp/test_ac_skills_empty/bot/skills");

    char *skills = agent_load_skills("/tmp/test_ac_skills_empty", "bot");
    assert(skills == NULL);

    system("rm -rf /tmp/test_ac_skills_empty");
    printf("  PASS: agent load skills empty dir\n");
}

static void test_build_system_prompt(void) {
    /* Setup: agents dir with system.md and skills */
    system("rm -rf /tmp/test_ac_build");
    make_dir("/tmp/test_ac_build");
    make_dir("/tmp/test_ac_build/mybot");
    make_dir("/tmp/test_ac_build/mybot/skills");

    FILE *f = fopen("/tmp/test_ac_build/mybot/system.md", "w");
    fprintf(f, "You are {agent_name}. Session {session_id}. Date {date}.");
    fclose(f);

    f = fopen("/tmp/test_ac_build/mybot/skills/coding.md", "w");
    fprintf(f, "You can write code.");
    fclose(f);

    /* Open DB, seed agent, set soul+memory */
    unlink("/tmp/test_ac_build.db");
    sqlite3 *db = db_open("/tmp/test_ac_build.db");
    assert(db != NULL);

    AgentRow *row = db_agent_seed(db, "/tmp/test_ac_build", "mybot");
    assert(row != NULL);
    agent_row_free(row);

    /* Create memory blocks to replace old soul/memory columns */
    memory_block_create(db, "mybot", "persona", "Agent identity and tone", "I am friendly.", 5000);
    memory_block_create(db, "mybot", "human", "Known facts about the user", "User likes C.", 5000);

    /* Build prompt */
    Config cfg = {0};
    char *prompt = agent_build_system_prompt(db, "mybot", 7,
                                            "/tmp/test_ac_build", &cfg);
    assert(prompt != NULL);
    assert(strstr(prompt, "You are mybot.") != NULL);
    assert(strstr(prompt, "Session 7.") != NULL);
    assert(strstr(prompt, "I am friendly.") != NULL);
    assert(strstr(prompt, "User likes C.") != NULL);
    assert(strstr(prompt, "## Skills\nYou can write code.") != NULL);
    free(prompt);

    /* NULL agent_name falls back to config_render_system_prompt */
    cfg.system_prompt = strdup("fallback {session_id}");
    char *fb = agent_build_system_prompt(db, NULL, 5, "/tmp/test_ac_build", &cfg);
    assert(fb != NULL);
    assert(strstr(fb, "fallback 5") != NULL);
    free(fb);
    free(cfg.system_prompt);

    db_close(db);
    unlink("/tmp/test_ac_build.db");
    system("rm -rf /tmp/test_ac_build");
    printf("  PASS: agent build system prompt\n");
}

static void test_whitelist_add_remove(void) {
    /* T196: host management now uses cclaw.db agent_config table */
    unlink("/tmp/test_agent_wl.db");
    sqlite3 *db = db_open("/tmp/test_agent_wl.db");
    assert(db != NULL);

    /* Add host */
    assert(agent_config_add_host(db, "bot", "api.example.com") == 0);

    /* Verify */
    size_t count = 0;
    char **hosts = agent_config_get_hosts(db, "bot", &count);
    assert(count == 1);
    assert(strcmp(hosts[0], "api.example.com") == 0);
    free(hosts[0]); free(hosts);

    /* Add duplicate — should be no-op */
    assert(agent_config_add_host(db, "bot", "api.example.com") == 0);
    hosts = agent_config_get_hosts(db, "bot", &count);
    assert(count == 1);
    free(hosts[0]); free(hosts);

    /* Add second host */
    assert(agent_config_add_host(db, "bot", "api.github.com") == 0);
    hosts = agent_config_get_hosts(db, "bot", &count);
    assert(count == 2);
    free(hosts[0]); free(hosts[1]); free(hosts);

    /* Remove first host */
    assert(agent_config_remove_host(db, "bot", "api.example.com") == 0);
    hosts = agent_config_get_hosts(db, "bot", &count);
    assert(count == 1);
    assert(strcmp(hosts[0], "api.github.com") == 0);
    free(hosts[0]); free(hosts);

    /* Remove non-existent — should succeed */
    assert(agent_config_remove_host(db, "bot", "nope.com") == 0);

    /* Invalid args */
    assert(agent_config_add_host(db, "bot", "") == -1);

    db_close(db);
    unlink("/tmp/test_agent_wl.db");
    printf("  PASS test_whitelist_add_remove\n");
}

static void test_agent_config_db_roundtrip(void) {
    /* T196: save to DB, load from DB, verify fields match */
    unlink("/tmp/test_ac_db.db");
    sqlite3 *db = db_open("/tmp/test_ac_db.db");
    assert(db != NULL);

    AgentConfig ac = {0};
    ac.name = "coder";
    ac.model = "anthropic/claude-sonnet";
    ac.workspace = "/home/coder/work";
    ac.max_iterations = 50;
    char *tools[] = {"shell_exec", "file_read", "file_write"};
    ac.tools = tools;
    ac.tool_count = 3;
    char *hosts[] = {"api.github.com", "example.com"};
    ac.allowed_hosts = hosts;
    ac.allowed_hosts_count = 2;

    assert(agent_config_save_db(db, &ac) == 0);

    AgentConfig *loaded = agent_config_load_db(db, "coder");
    assert(loaded != NULL);
    assert(strcmp(loaded->name, "coder") == 0);
    assert(strcmp(loaded->model, "anthropic/claude-sonnet") == 0);
    assert(strcmp(loaded->workspace, "/home/coder/work") == 0);
    assert(loaded->max_iterations == 50);
    assert(loaded->tool_count == 3);
    assert(strcmp(loaded->tools[0], "shell_exec") == 0);
    assert(loaded->allowed_hosts_count == 2);
    assert(strcmp(loaded->allowed_hosts[0], "api.github.com") == 0);
    agent_config_free(loaded);

    /* Non-existent agent returns NULL */
    assert(agent_config_load_db(db, "ghost") == NULL);

    db_close(db);
    unlink("/tmp/test_ac_db.db");
    printf("  PASS: agent config DB roundtrip (T196)\n");
}

static void test_agent_config_migrate(void) {
    /* T196: migrate agent.json → cclaw.db, verify file deleted */
    system("rm -rf /tmp/test_ac_migrate");
    make_dir("/tmp/test_ac_migrate");
    make_dir("/tmp/test_ac_migrate/coder");
    write_file("/tmp/test_ac_migrate/coder/agent.json",
        "{\"model\":\"openai/gpt-4o\",\"max_iterations\":30,"
        "\"allowed_hosts\":[\"api.github.com\"]}");

    unlink("/tmp/test_ac_migrate.db");
    sqlite3 *db = db_open("/tmp/test_ac_migrate.db");
    assert(db != NULL);

    assert(agent_config_migrate_json(db, "/tmp/test_ac_migrate", "coder") == 0);

    /* Verify config in DB */
    AgentConfig *loaded = agent_config_load_db(db, "coder");
    assert(loaded != NULL);
    assert(strcmp(loaded->model, "openai/gpt-4o") == 0);
    assert(loaded->max_iterations == 30);
    assert(loaded->allowed_hosts_count == 1);
    assert(strcmp(loaded->allowed_hosts[0], "api.github.com") == 0);
    agent_config_free(loaded);

    /* Verify agent.json deleted */
    FILE *f = fopen("/tmp/test_ac_migrate/coder/agent.json", "r");
    assert(f == NULL);

    /* Migrate non-existent file — should succeed (no-op) */
    assert(agent_config_migrate_json(db, "/tmp/test_ac_migrate", "ghost") == 0);

    db_close(db);
    unlink("/tmp/test_ac_migrate.db");
    system("rm -rf /tmp/test_ac_migrate");
    printf("  PASS: agent config migrate JSON (T196)\n");
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
    test_agent_load_skills();
    test_agent_load_skills_missing_dir();
    test_agent_load_skills_empty();
    test_build_system_prompt();
    test_whitelist_add_remove();
    test_agent_config_db_roundtrip();
    test_agent_config_migrate();
    printf("All tests passed.\n");
    return 0;
}
