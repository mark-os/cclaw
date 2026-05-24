#include "agent_config.h"
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

int main(void) {
    printf("test_agent_config:\n");
    test_discover_agents();
    test_discover_empty();
    test_discover_missing_dir();
    printf("All tests passed.\n");
    return 0;
}
