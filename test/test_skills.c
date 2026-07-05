/* Test: skill discovery, frontmatter parsing, and index injection
 * (SKILL.md convention — progressive disclosure, body never in prompt). */
#define _POSIX_C_SOURCE 200809L
#include "skills.h"
#include "config_registry.h"
#include "agent_config.h"
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define DB_PATH "/tmp/test_skills.db"
#define ROOT "/tmp/test_skills_dirs"

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void setup_tree(void) {
    system("rm -rf " ROOT);
    mkdir(ROOT, 0755);
    mkdir(ROOT "/global", 0755);
    mkdir(ROOT "/global/weather", 0755);
    write_file(ROOT "/global/weather/SKILL.md",
        "---\n"
        "name: weather\n"
        "description: \"Current weather and forecasts for any location\"\n"
        "metadata:\n"
        "  openclaw:\n"
        "    emoji: cloud\n"
        "---\n"
        "# Weather skill body\nSECRET-BODY-MARKER\n");
    /* directory without frontmatter description → skipped */
    mkdir(ROOT "/global/broken", 0755);
    write_file(ROOT "/global/broken/SKILL.md", "no frontmatter here\n");
    /* bare .md skill with frontmatter, no name → filename fallback */
    write_file(ROOT "/global/notes.md",
        "---\ndescription: Take structured notes\n---\nbody\n");
    /* agent-personal dir with a colliding + a unique skill */
    mkdir(ROOT "/agents", 0755);
    mkdir(ROOT "/agents/Ag", 0755);
    mkdir(ROOT "/agents/Ag/skills", 0755);
    mkdir(ROOT "/agents/Ag/skills/weather", 0755);
    write_file(ROOT "/agents/Ag/skills/weather/SKILL.md",
        "---\nname: weather\ndescription: agent-local weather (shadowed)\n---\nx\n");
    mkdir(ROOT "/agents/Ag/skills/deploy", 0755);
    write_file(ROOT "/agents/Ag/skills/deploy/SKILL.md",
        "---\nname: deploy\ndescription: Deploy the service\n---\nx\n");
}

static void test_frontmatter(void) {
    char *name = NULL, *desc = NULL;
    assert(skill_frontmatter_parse(ROOT "/global/weather/SKILL.md",
                                   &name, &desc) == 0);
    assert(name && strcmp(name, "weather") == 0);
    assert(desc && strcmp(desc, "Current weather and forecasts for any location") == 0);
    free(name); free(desc);

    assert(skill_frontmatter_parse(ROOT "/global/broken/SKILL.md",
                                   &name, &desc) == -1);
    assert(name == NULL && desc == NULL);

    /* no name field: parse succeeds, name stays NULL (caller fallback) */
    assert(skill_frontmatter_parse(ROOT "/global/notes.md", &name, &desc) == 0);
    assert(name == NULL);
    assert(desc && strcmp(desc, "Take structured notes") == 0);
    free(desc);
    printf("  PASS test_frontmatter\n");
}

static void test_dirs_resolve(sqlite3 *db) {
    size_t n = 0;
    char **dirs = skills_dirs_resolve(db, ROOT "/agents", "Ag", &n);
    assert(dirs && n == 2);  /* configured global dir + per-agent dir */
    assert(strcmp(dirs[0], ROOT "/global") == 0);
    assert(strcmp(dirs[1], ROOT "/agents/Ag/skills") == 0);
    skills_dirs_free(dirs, n);

    /* nonexistent config dirs are skipped; missing agent dir too */
    config_set(db, "skills_dirs", "[\"/nonexistent_xyz\"]");
    dirs = skills_dirs_resolve(db, ROOT "/agents", "NoSuchAgent", &n);
    assert(dirs == NULL && n == 0);
    config_set(db, "skills_dirs", "[\"" ROOT "/global\"]");
    printf("  PASS test_dirs_resolve\n");
}

static void test_index(sqlite3 *db) {
    char *ix = skills_index_build(db, ROOT "/agents", "Ag");
    assert(ix != NULL);
    /* index entries present with name/description/location */
    assert(strstr(ix, "<available_skills>"));
    assert(strstr(ix, "<name>weather</name>"));
    assert(strstr(ix, "Current weather and forecasts"));
    assert(strstr(ix, ROOT "/global/weather/SKILL.md"));
    assert(strstr(ix, "<name>deploy</name>"));
    /* bare .md skill, filename fallback name */
    assert(strstr(ix, "<name>notes</name>"));
    /* first name wins: agent-local weather shadowed by global */
    assert(strstr(ix, "agent-local weather") == NULL);
    /* skipped: no-frontmatter dir */
    assert(strstr(ix, "broken") == NULL);
    /* progressive disclosure: body text never enters the prompt */
    assert(strstr(ix, "SECRET-BODY-MARKER") == NULL);
    free(ix);
    printf("  PASS test_index\n");
}

/* Skills declared by an attached extension's manifest ($.skills[]) appear in
 * the index; scanned dirs shadow them (first name wins). */
static void test_extension_skills(sqlite3 *db) {
    mkdir(ROOT "/store", 0755);
    mkdir(ROOT "/store/pack", 0755);
    mkdir(ROOT "/store/pack/skills", 0755);
    mkdir(ROOT "/store/pack/skills/packaged", 0755);
    write_file(ROOT "/store/pack/skills/packaged/SKILL.md",
        "---\ndescription: Skill shipped by an extension\n---\nEXT-BODY\n");
    mkdir(ROOT "/store/pack/skills/weather", 0755);
    write_file(ROOT "/store/pack/skills/weather/SKILL.md",
        "---\nname: weather\ndescription: extension weather (shadowed)\n---\nx\n");
    write_file(ROOT "/store/pack/extension.json",
        "{\"name\":\"pack\",\"skills\":[\"skills/packaged\",\"skills/weather\"]}");
    char *sql = sqlite3_mprintf(
        "INSERT INTO extensions(name, path, owner_agent, published) "
        "VALUES('pack', '%q/store/pack', 'Ag', 0);"
        "INSERT INTO agent_extensions(agent_name, extension_name, enabled) "
        "VALUES('Ag', 'pack', 1);", ROOT);
    assert(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_free(sql);

    char *ix = skills_index_build(db, ROOT "/agents", "Ag");
    assert(ix != NULL);
    assert(strstr(ix, "<name>packaged</name>"));
    assert(strstr(ix, ROOT "/store/pack/skills/packaged/SKILL.md"));
    /* dir-scanned weather wins over the extension's weather */
    assert(strstr(ix, "extension weather") == NULL);
    free(ix);

    /* unattached agents don't see it */
    sqlite3_exec(db, "UPDATE agent_extensions SET enabled=0 WHERE extension_name='pack'",
                 NULL, NULL, NULL);
    ix = skills_index_build(db, ROOT "/agents", "Ag");
    assert(ix && strstr(ix, "packaged") == NULL);
    free(ix);
    sqlite3_exec(db, "UPDATE agent_extensions SET enabled=1 WHERE extension_name='pack'",
                 NULL, NULL, NULL);
    printf("  PASS test_extension_skills\n");
}

static void test_no_skills(sqlite3 *db) {
    config_set(db, "skills_dirs", "[]");
    char *ix = skills_index_build(db, ROOT "/agents", "NoSuchAgent");
    assert(ix == NULL);
    config_set(db, "skills_dirs", "[\"" ROOT "/global\"]");
    printf("  PASS test_no_skills\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    alarm(10);
    printf("test_skills:\n");
    unlink(DB_PATH);
    setup_tree();

    sqlite3 *db = test_db_open(DB_PATH);
    assert(db);
    assert(config_registry_sync(db) == 0);
    /* point discovery at the test tree, not the real ~ dirs */
    assert(config_set(db, "skills_dirs", "[\"" ROOT "/global\"]") == 0);

    test_frontmatter();
    test_dirs_resolve(db);
    test_index(db);
    test_extension_skills(db);
    test_no_skills(db);

    db_close(db);
    unlink(DB_PATH);
    system("rm -rf " ROOT);
    printf("All skills tests passed.\n");
    return 0;
}
