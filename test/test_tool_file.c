#define _POSIX_C_SOURCE 200809L
#include "tool_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[256];

static void setup(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cclaw_test_file_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);

    /* Create a test file */
    char path[512];
    snprintf(path, sizeof(path), "%s/hello.txt", tmpdir);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "hello world");
    fclose(f);

    /* Create a subdir */
    char subdir[512];
    snprintf(subdir, sizeof(subdir), "%s/sub", tmpdir);
    mkdir(subdir, 0755);
    snprintf(path, sizeof(path), "%s/sub/nested.txt", tmpdir);
    f = fopen(path, "w");
    assert(f);
    fprintf(f, "nested content");
    fclose(f);
}

static void cleanup(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    system(cmd);
}

static void test_basic_read(void) {
    char args[256];
    snprintf(args, sizeof(args), "{\"path\":\"hello.txt\"}");
    char *r = tool_file_read_handler(args, (void *)tmpdir);
    assert(r != NULL);
    assert(strcmp(r, "hello world") == 0);
    free(r);
    printf("  PASS test_basic_read\n");
}

static void test_nested_read(void) {
    char args[256];
    snprintf(args, sizeof(args), "{\"path\":\"sub/nested.txt\"}");
    char *r = tool_file_read_handler(args, (void *)tmpdir);
    assert(r != NULL);
    assert(strcmp(r, "nested content") == 0);
    free(r);
    printf("  PASS test_nested_read\n");
}

static void test_path_traversal_blocked(void) {
    /* V1: attempt to escape workspace via ../ */
    char *r = tool_file_read_handler("{\"path\":\"../../../etc/passwd\"}", (void *)tmpdir);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_path_traversal_blocked\n");
}

static void test_absolute_path_outside(void) {
    /* V1: absolute path outside workspace */
    char *r = tool_file_read_handler("{\"path\":\"/etc/hostname\"}", (void *)tmpdir);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_absolute_path_outside\n");
}

static void test_missing_file(void) {
    char *r = tool_file_read_handler("{\"path\":\"nonexistent.txt\"}", (void *)tmpdir);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_missing_file\n");
}

static void test_invalid_json(void) {
    char *r = tool_file_read_handler("not json", (void *)tmpdir);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_invalid_json\n");
}

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_file_read_register(&reg, tmpdir);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "file_read");
    assert(e != NULL);
    assert(e->handler == tool_file_read_handler);
    tools_free(&reg);
    printf("  PASS test_register\n");
}

int main(void) {
    printf("test_tool_file:\n");
    setup();
    test_basic_read();
    test_nested_read();
    test_path_traversal_blocked();
    test_absolute_path_outside();
    test_missing_file();
    test_invalid_json();
    test_register();
    cleanup();
    printf("All file_read tool tests passed.\n");
    return 0;
}
