#define _POSIX_C_SOURCE 200809L
#include "tool_file.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[256];
static FileReadCtx file_ctx;

static void setup(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cclaw_test_file_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);
    file_ctx.workspace = tmpdir;
    file_ctx.extra_read_path = NULL;

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
    char *r = tool_file_read_handler(args, (void *)&file_ctx);
    assert(r != NULL);
    assert(strcmp(r, "hello world") == 0);
    free(r);
    printf("  PASS test_basic_read\n");
}

static void test_nested_read(void) {
    char args[256];
    snprintf(args, sizeof(args), "{\"path\":\"sub/nested.txt\"}");
    char *r = tool_file_read_handler(args, (void *)&file_ctx);
    assert(r != NULL);
    assert(strcmp(r, "nested content") == 0);
    free(r);
    printf("  PASS test_nested_read\n");
}

static void test_path_traversal_blocked(void) {
    /* V1: attempt to escape workspace via ../ */
    char *r = tool_file_read_handler("{\"path\":\"../../../etc/passwd\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_path_traversal_blocked\n");
}

static void test_absolute_path_outside(void) {
    /* V1: absolute path outside workspace */
    char *r = tool_file_read_handler("{\"path\":\"/etc/hostname\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_absolute_path_outside\n");
}

static void test_missing_file(void) {
    char *r = tool_file_read_handler("{\"path\":\"nonexistent.txt\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_missing_file\n");
}

static void test_invalid_json(void) {
    char *r = tool_file_read_handler("not json", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_invalid_json\n");
}

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_file_read_register(&reg, &file_ctx);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "file_read");
    assert(e != NULL);
    assert(e->handler == tool_file_read_handler);
    tools_free(&reg);
    printf("  PASS test_register\n");
}

/* file_write tests */

static void test_write_new_file(void) {
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"newfile.txt\",\"content\":\"hello write\"}");
    char *r = tool_file_write_handler(args, (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "wrote") != NULL);
    free(r);

    /* Verify content via read */
    r = tool_file_read_handler("{\"path\":\"newfile.txt\"}", (void *)&file_ctx);
    assert(strcmp(r, "hello write") == 0);
    free(r);
    printf("  PASS test_write_new_file\n");
}

static void test_write_overwrite(void) {
    char *r = tool_file_write_handler("{\"path\":\"hello.txt\",\"content\":\"overwritten\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "wrote") != NULL);
    free(r);

    r = tool_file_read_handler("{\"path\":\"hello.txt\"}", (void *)&file_ctx);
    assert(strcmp(r, "overwritten") == 0);
    free(r);
    printf("  PASS test_write_overwrite\n");
}

static void test_write_nested(void) {
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"sub/written.txt\",\"content\":\"nested write\"}");
    char *r = tool_file_write_handler(args, (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "wrote") != NULL);
    free(r);

    r = tool_file_read_handler("{\"path\":\"sub/written.txt\"}", (void *)&file_ctx);
    assert(strcmp(r, "nested write") == 0);
    free(r);
    printf("  PASS test_write_nested\n");
}

static void test_write_traversal_blocked(void) {
    /* V1: path escape via ../ */
    char *r = tool_file_write_handler("{\"path\":\"../../evil.txt\",\"content\":\"bad\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_write_traversal_blocked\n");
}

static void test_write_absolute_outside(void) {
    /* V1: absolute path outside workspace */
    char *r = tool_file_write_handler("{\"path\":\"/tmp/evil.txt\",\"content\":\"bad\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_write_absolute_outside\n");
}

static void test_write_missing_content(void) {
    char *r = tool_file_write_handler("{\"path\":\"foo.txt\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_write_missing_content\n");
}

static void test_write_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_file_write_register(&reg, &file_ctx);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "file_write");
    assert(e != NULL);
    assert(e->handler == tool_file_write_handler);
    tools_free(&reg);
    printf("  PASS test_write_register\n");
}

static void test_list_basic(void) {
    char *r = tool_file_list_handler("{\"path\":\".\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "hello.txt") != NULL);
    assert(strstr(r, "sub/") != NULL);  /* directories get a trailing slash */
    free(r);
    printf("  PASS test_list_basic\n");
}

static void test_list_default_path(void) {
    /* No path → defaults to "." */
    char *r = tool_file_list_handler("{}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "hello.txt") != NULL);
    free(r);
    printf("  PASS test_list_default_path\n");
}

static void test_list_outside_blocked(void) {
    char *r = tool_file_list_handler("{\"path\":\"/etc\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_list_outside_blocked\n");
}

static void test_find_recursive(void) {
    /* '*.txt' (no slash) matches the basename at any depth */
    char *r = tool_file_find_handler("{\"pattern\":\"*.txt\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "hello.txt") != NULL);
    assert(strstr(r, "sub/nested.txt") != NULL);
    free(r);
    printf("  PASS test_find_recursive\n");
}

static void test_find_globstar_path(void) {
    /* A '/'-bearing pattern matches the relative path; '**' crosses dirs */
    char *r = tool_file_find_handler("{\"pattern\":\"sub/**/*.txt\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "sub/nested.txt") != NULL);
    assert(strstr(r, "hello.txt\n") == NULL && strcmp(r, "hello.txt") != 0);  /* root file excluded */
    free(r);
    printf("  PASS test_find_globstar_path\n");
}

static void test_find_no_match(void) {
    char *r = tool_file_find_handler("{\"pattern\":\"*.zzz\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "No files found") != NULL);
    free(r);
    printf("  PASS test_find_no_match\n");
}

static void test_find_missing_pattern(void) {
    char *r = tool_file_find_handler("{}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_find_missing_pattern\n");
}

static void test_edit_batch(void) {
    /* Two non-overlapping edits, matched against the original content */
    char *r = tool_file_write_handler(
        "{\"path\":\"edit.txt\",\"content\":\"alpha\\nbeta\\ngamma\"}", (void *)&file_ctx);
    free(r);
    r = tool_file_edit_handler(
        "{\"path\":\"edit.txt\",\"edits\":["
        "{\"oldText\":\"alpha\",\"newText\":\"AAA\"},"
        "{\"oldText\":\"gamma\",\"newText\":\"GGG\"}]}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "applied 2 edits") != NULL);
    free(r);
    r = tool_file_read_handler("{\"path\":\"edit.txt\"}", (void *)&file_ctx);
    assert(strcmp(r, "AAA\nbeta\nGGG") == 0);
    free(r);
    printf("  PASS test_edit_batch\n");
}

static void test_edit_not_unique(void) {
    char *r = tool_file_write_handler(
        "{\"path\":\"dup.txt\",\"content\":\"x x x\"}", (void *)&file_ctx);
    free(r);
    r = tool_file_edit_handler(
        "{\"path\":\"dup.txt\",\"edits\":[{\"oldText\":\"x\",\"newText\":\"y\"}]}",
        (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "not unique") != NULL);
    free(r);
    printf("  PASS test_edit_not_unique\n");
}

static void test_edit_not_found(void) {
    char *r = tool_file_edit_handler(
        "{\"path\":\"hello.txt\",\"edits\":[{\"oldText\":\"zzz\",\"newText\":\"y\"}]}",
        (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "not found") != NULL);
    free(r);
    printf("  PASS test_edit_not_found\n");
}

static void test_edit_overlap(void) {
    char *r = tool_file_write_handler(
        "{\"path\":\"ov.txt\",\"content\":\"abcdef\"}", (void *)&file_ctx);
    free(r);
    r = tool_file_edit_handler(
        "{\"path\":\"ov.txt\",\"edits\":["
        "{\"oldText\":\"abcd\",\"newText\":\"X\"},"
        "{\"oldText\":\"cdef\",\"newText\":\"Y\"}]}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "overlapping") != NULL);
    free(r);
    printf("  PASS test_edit_overlap\n");
}

/* --- file_grep tests --- */

static void test_grep_basic(void) {
    /* Create a file with known content for grep */
    char *w = tool_file_write_handler(
        "{\"path\":\"grep_target.txt\",\"content\":\"alpha\\nbeta\\ngamma\"}", (void *)&file_ctx);
    free(w);
    char *r = tool_file_grep_handler("{\"pattern\":\"beta\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "grep_target.txt:2:beta") != NULL);
    free(r);
    printf("  PASS test_grep_basic\n");
}

static void test_grep_glob_filter(void) {
    /* glob restricts to matching basenames */
    char *w = tool_file_write_handler(
        "{\"path\":\"src.c\",\"content\":\"findme here\"}", (void *)&file_ctx);
    free(w);
    w = tool_file_write_handler(
        "{\"path\":\"src.py\",\"content\":\"findme there\"}", (void *)&file_ctx);
    free(w);
    char *r = tool_file_grep_handler(
        "{\"pattern\":\"findme\",\"glob\":\"*.c\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "src.c:1:findme here") != NULL);
    assert(strstr(r, "src.py") == NULL);
    free(r);
    printf("  PASS test_grep_glob_filter\n");
}

static void test_grep_recursive(void) {
    /* Should find matches in subdirectories */
    char *w = tool_file_write_handler(
        "{\"path\":\"sub/deep.txt\",\"content\":\"unique_marker\"}", (void *)&file_ctx);
    free(w);
    char *r = tool_file_grep_handler("{\"pattern\":\"unique_marker\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strstr(r, "sub/deep.txt:1:unique_marker") != NULL);
    free(r);
    printf("  PASS test_grep_recursive\n");
}

static void test_grep_invalid_regex(void) {
    char *r = tool_file_grep_handler("{\"pattern\":\"[invalid\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strcmp(r, "error: invalid regex") == 0);
    free(r);
    printf("  PASS test_grep_invalid_regex\n");
}

static void test_grep_no_match(void) {
    char *r = tool_file_grep_handler("{\"pattern\":\"zzzzz\"}", (void *)&file_ctx);
    assert(r != NULL);
    assert(strcmp(r, "No matches found") == 0);
    free(r);
    printf("  PASS test_grep_no_match\n");
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
    test_write_new_file();
    test_write_overwrite();
    test_write_nested();
    test_write_traversal_blocked();
    test_write_absolute_outside();
    test_write_missing_content();
    test_write_register();
    test_list_basic();
    test_list_default_path();
    test_list_outside_blocked();
    test_find_recursive();
    test_find_globstar_path();
    test_find_no_match();
    test_find_missing_pattern();
    test_edit_batch();
    test_edit_not_unique();
    test_edit_not_found();
    test_edit_overlap();
    test_grep_basic();
    test_grep_glob_filter();
    test_grep_recursive();
    test_grep_invalid_regex();
    test_grep_no_match();
    cleanup();
    printf("All file tool tests passed.\n");
    return 0;
}
