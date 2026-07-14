#define _POSIX_C_SOURCE 200809L
#include "test_util.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char tmpdir[256];
static FileReadCtx file_ctx;

/* Sandboxed context for escape tests — uses forked namespace path */

static void setup(void) {
    snprintf(tmpdir, sizeof(tmpdir), "/tmp/cclaw_test_file_XXXXXX");
    assert(mkdtemp(tmpdir) != NULL);

    /* In-process (host) context — for roundtrip tests */
    memset(&file_ctx, 0, sizeof(file_ctx));
    file_ctx.workspace = tmpdir;

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
    char *r = test_file_tool_run("file_read", args, &file_ctx);
    assert(r != NULL);
    assert(strcmp(r, "hello world") == 0);
    free(r);
    printf("  PASS test_basic_read\n");
}

static void test_nested_read(void) {
    char args[256];
    snprintf(args, sizeof(args), "{\"path\":\"sub/nested.txt\"}");
    char *r = test_file_tool_run("file_read", args, &file_ctx);
    assert(r != NULL);
    assert(strcmp(r, "nested content") == 0);
    free(r);
    printf("  PASS test_nested_read\n");
}



static void test_missing_file(void) {
    char *r = test_file_tool_run("file_read", "{\"path\":\"nonexistent.txt\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_missing_file\n");
}

static void test_invalid_json(void) {
    char *r = test_file_tool_run("file_read", "not json", &file_ctx);
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
    assert(e->handler == tool_sandboxed_stub);
    tools_free(&reg);
    printf("  PASS test_register\n");
}

/* file_write tests */

static void test_write_new_file(void) {
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"newfile.txt\",\"content\":\"hello write\"}");
    char *r = test_file_tool_run("file_write", args, &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "wrote") != NULL);
    free(r);

    r = test_file_tool_run("file_read", "{\"path\":\"newfile.txt\"}", &file_ctx);
    assert(strcmp(r, "hello write") == 0);
    free(r);
    printf("  PASS test_write_new_file\n");
}

static void test_write_overwrite(void) {
    char *r = test_file_tool_run("file_write", "{\"path\":\"hello.txt\",\"content\":\"overwritten\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "wrote") != NULL);
    free(r);

    r = test_file_tool_run("file_read", "{\"path\":\"hello.txt\"}", &file_ctx);
    assert(strcmp(r, "overwritten") == 0);
    free(r);
    printf("  PASS test_write_overwrite\n");
}

static void test_write_nested(void) {
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"sub/written.txt\",\"content\":\"nested write\"}");
    char *r = test_file_tool_run("file_write", args, &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "wrote") != NULL);
    free(r);

    r = test_file_tool_run("file_read", "{\"path\":\"sub/written.txt\"}", &file_ctx);
    assert(strcmp(r, "nested write") == 0);
    free(r);
    printf("  PASS test_write_nested\n");
}



static void test_write_missing_content(void) {
    char *r = test_file_tool_run("file_write", "{\"path\":\"foo.txt\"}", &file_ctx);
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
    assert(e->handler == tool_sandboxed_stub);
    tools_free(&reg);
    printf("  PASS test_write_register\n");
}

static void test_list_basic(void) {
    char *r = test_file_tool_run("file_list", "{\"path\":\".\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "hello.txt") != NULL);
    assert(strstr(r, "sub/") != NULL);
    free(r);
    printf("  PASS test_list_basic\n");
}

static void test_list_default_path(void) {
    char *r = test_file_tool_run("file_list", "{}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "hello.txt") != NULL);
    free(r);
    printf("  PASS test_list_default_path\n");
}


static void test_find_recursive(void) {
    char *r = test_file_tool_run("file_find", "{\"pattern\":\"*.txt\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "hello.txt") != NULL);
    assert(strstr(r, "sub/nested.txt") != NULL);
    free(r);
    printf("  PASS test_find_recursive\n");
}

static void test_find_globstar_path(void) {
    char *r = test_file_tool_run("file_find", "{\"pattern\":\"sub/**/*.txt\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "sub/nested.txt") != NULL);
    assert(strstr(r, "hello.txt\n") == NULL && strcmp(r, "hello.txt") != 0);
    free(r);
    printf("  PASS test_find_globstar_path\n");
}

static void test_find_no_match(void) {
    char *r = test_file_tool_run("file_find", "{\"pattern\":\"*.zzz\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "No files found") != NULL);
    free(r);
    printf("  PASS test_find_no_match\n");
}

static void test_find_missing_pattern(void) {
    char *r = test_file_tool_run("file_find", "{}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "error") != NULL);
    free(r);
    printf("  PASS test_find_missing_pattern\n");
}

static void test_edit_batch(void) {
    char *r = test_file_tool_run("file_write",
        "{\"path\":\"edit.txt\",\"content\":\"alpha\\nbeta\\ngamma\"}", &file_ctx);
    free(r);
    r = test_file_tool_run("file_edit",
        "{\"path\":\"edit.txt\",\"edits\":["
        "{\"oldText\":\"alpha\",\"newText\":\"AAA\"},"
        "{\"oldText\":\"gamma\",\"newText\":\"GGG\"}]}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "applied 2 edits") != NULL);
    free(r);
    r = test_file_tool_run("file_read", "{\"path\":\"edit.txt\"}", &file_ctx);
    assert(strcmp(r, "AAA\nbeta\nGGG") == 0);
    free(r);
    printf("  PASS test_edit_batch\n");
}

static void test_edit_not_unique(void) {
    char *r = test_file_tool_run("file_write",
        "{\"path\":\"dup.txt\",\"content\":\"x x x\"}", &file_ctx);
    free(r);
    r = test_file_tool_run("file_edit",
        "{\"path\":\"dup.txt\",\"edits\":[{\"oldText\":\"x\",\"newText\":\"y\"}]}",
        &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "not unique") != NULL);
    free(r);
    printf("  PASS test_edit_not_unique\n");
}

static void test_edit_not_found(void) {
    char *r = test_file_tool_run("file_edit",
        "{\"path\":\"hello.txt\",\"edits\":[{\"oldText\":\"zzz\",\"newText\":\"y\"}]}",
        &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "not found") != NULL);
    free(r);
    printf("  PASS test_edit_not_found\n");
}

static void test_edit_overlap(void) {
    char *r = test_file_tool_run("file_write",
        "{\"path\":\"ov.txt\",\"content\":\"abcdef\"}", &file_ctx);
    free(r);
    r = test_file_tool_run("file_edit",
        "{\"path\":\"ov.txt\",\"edits\":["
        "{\"oldText\":\"abcd\",\"newText\":\"X\"},"
        "{\"oldText\":\"cdef\",\"newText\":\"Y\"}]}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "overlapping") != NULL);
    free(r);
    printf("  PASS test_edit_overlap\n");
}

/* --- file_grep tests --- */

static void test_grep_basic(void) {
    char *w = test_file_tool_run("file_write",
        "{\"path\":\"grep_target.txt\",\"content\":\"alpha\\nbeta\\ngamma\"}", &file_ctx);
    free(w);
    char *r = test_file_tool_run("file_grep", "{\"pattern\":\"beta\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "grep_target.txt:2:beta") != NULL);
    free(r);
    printf("  PASS test_grep_basic\n");
}

static void test_grep_glob_filter(void) {
    char *w = test_file_tool_run("file_write",
        "{\"path\":\"src.c\",\"content\":\"findme here\"}", &file_ctx);
    free(w);
    w = test_file_tool_run("file_write",
        "{\"path\":\"src.py\",\"content\":\"findme there\"}", &file_ctx);
    free(w);
    char *r = test_file_tool_run("file_grep",
        "{\"pattern\":\"findme\",\"glob\":\"*.c\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "src.c:1:findme here") != NULL);
    assert(strstr(r, "src.py") == NULL);
    free(r);
    printf("  PASS test_grep_glob_filter\n");
}

static void test_grep_recursive(void) {
    char *w = test_file_tool_run("file_write",
        "{\"path\":\"sub/deep.txt\",\"content\":\"unique_marker\"}", &file_ctx);
    free(w);
    char *r = test_file_tool_run("file_grep", "{\"pattern\":\"unique_marker\"}", &file_ctx);
    assert(r != NULL);
    assert(strstr(r, "sub/deep.txt:1:unique_marker") != NULL);
    free(r);
    printf("  PASS test_grep_recursive\n");
}

static void test_grep_invalid_regex(void) {
    char *r = test_file_tool_run("file_grep", "{\"pattern\":\"[invalid\"}", &file_ctx);
    assert(r != NULL);
    assert(strcmp(r, "error: invalid regex") == 0);
    free(r);
    printf("  PASS test_grep_invalid_regex\n");
}

static void test_grep_no_match(void) {
    char *r = test_file_tool_run("file_grep", "{\"pattern\":\"zzzzz\"}", &file_ctx);
    assert(r != NULL);
    assert(strcmp(r, "No matches found") == 0);
    free(r);
    printf("  PASS test_grep_no_match\n");
}

/* ── path_grant_hint tests ─────────────────────────────────────────────── */

static void test_hint_outside_granted(void) {
    FileReadCtx hctx = {0};
    hctx.workspace = tmpdir;
    hctx.sb.sandbox = 1;
    char *r = test_file_tool_run("file_read", "{\"path\":\"/definitely/not/granted/file.txt\"}", &hctx);
    assert(r != NULL);
    assert(strstr(r, "outside your granted areas") != NULL);
    assert(strstr(r, "\"action\":\"request_changes\"") != NULL);
    assert(strstr(r, "\"read_paths\"") != NULL);
    free(r);
    printf("  PASS test_hint_outside_granted\n");
}

static void test_no_hint_inside_workspace(void) {
    FileReadCtx hctx = {0};
    hctx.workspace = tmpdir;
    hctx.sb.sandbox = 1;
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s/missing.txt\"}", tmpdir);
    char *r = test_file_tool_run("file_read", args, &hctx);
    assert(r != NULL);
    assert(strcmp(r, "error: cannot open file") == 0);
    free(r);
    printf("  PASS test_no_hint_inside_workspace\n");
}

static void test_no_hint_when_unsandboxed(void) {
    FileReadCtx hctx2 = {0};
    hctx2.workspace = tmpdir;
    hctx2.sb.sandbox = 0;
    char *r = test_file_tool_run("file_read", "{\"path\":\"/definitely/not/granted/file.txt\"}", &hctx2);
    assert(r != NULL);
    assert(strcmp(r, "error: cannot open file") == 0);
    free(r);
    printf("  PASS test_no_hint_when_unsandboxed\n");
}

static void test_hint_read_path_grant(void) {
    FileReadCtx hctx = {0};
    hctx.workspace = tmpdir;
    hctx.sb.sandbox = 1;
    static char *rp[] = { "/granted/read" };
    hctx.sb.read_paths = rp;
    hctx.sb.read_path_count = 1;

    /* Inside granted read path — genuinely missing file, plain error */
    char *r = test_file_tool_run("file_read", "{\"path\":\"/granted/read/missing.txt\"}", &hctx);
    assert(r != NULL);
    assert(strcmp(r, "error: cannot open file") == 0);
    free(r);

    /* Component-boundary mismatch — "/granted/readother" is NOT under "/granted/read" */
    r = test_file_tool_run("file_read", "{\"path\":\"/granted/readother/x.txt\"}", &hctx);
    assert(r != NULL);
    assert(strstr(r, "outside your granted areas") != NULL);
    free(r);

    printf("  PASS test_hint_read_path_grant\n");
}

static void test_hint_write_mode(void) {
    FileReadCtx hctx = {0};
    hctx.workspace = tmpdir;
    hctx.sb.sandbox = 1;

    /* Write to ungranted path — hint requests write_paths */
    char *r = test_file_tool_run("file_write", "{\"path\":\"/nope/out.txt\",\"content\":\"x\"}", &hctx);
    assert(r != NULL);
    assert(strstr(r, "outside your granted areas") != NULL);
    assert(strstr(r, "\"write_paths\":[\"/nope\"]") != NULL);
    free(r);

    /* List ungranted dir — hint suggests the dir itself under read_paths */
    r = test_file_tool_run("file_list", "{\"path\":\"/nope\"}", &hctx);
    assert(r != NULL);
    assert(strstr(r, "outside your granted areas") != NULL);
    assert(strstr(r, "\"read_paths\":[\"/nope\"]") != NULL);
    free(r);

    printf("  PASS test_hint_write_mode\n");
}

int main(void) {
    printf("test_tool_file:\n");
    setup();
    test_basic_read();
    test_nested_read();
    test_missing_file();
    test_invalid_json();
    test_register();
    test_write_new_file();
    test_write_overwrite();
    test_write_nested();
    test_write_missing_content();
    test_write_register();
    test_list_basic();
    test_list_default_path();
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
    test_hint_outside_granted();
    test_no_hint_inside_workspace();
    test_no_hint_when_unsandboxed();
    test_hint_read_path_grant();
    test_hint_write_mode();
    cleanup();
    printf("All file tool tests passed.\n");
    return 0;
}
