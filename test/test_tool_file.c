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

/* One pass over every file tool: write, read back, list, find, grep. */
static void test_smoke(void) {
    char *r = test_file_tool_run("file_read", "{\"path\":\"hello.txt\"}", &file_ctx);
    assert(strcmp(r, "hello world") == 0);
    free(r);

    r = test_file_tool_run("file_write",
        "{\"path\":\"sub/written.txt\",\"content\":\"unique_marker\\nline2\"}", &file_ctx);
    assert(strstr(r, "wrote") != NULL);
    free(r);
    r = test_file_tool_run("file_read", "{\"path\":\"sub/written.txt\"}", &file_ctx);
    assert(strcmp(r, "unique_marker\nline2") == 0);
    free(r);

    r = test_file_tool_run("file_list", "{}", &file_ctx);
    assert(strstr(r, "hello.txt") != NULL && strstr(r, "sub/") != NULL);
    free(r);

    r = test_file_tool_run("file_find", "{\"pattern\":\"*.txt\"}", &file_ctx);
    assert(strstr(r, "hello.txt") != NULL && strstr(r, "sub/nested.txt") != NULL);
    free(r);

    r = test_file_tool_run("file_grep", "{\"pattern\":\"line2\"}", &file_ctx);
    assert(strstr(r, "sub/written.txt:2:line2") != NULL);
    free(r);

    r = test_file_tool_run("file_read", "{\"path\":\"nonexistent.txt\"}", &file_ctx);
    assert(strstr(r, "error") != NULL);
    free(r);
}

/* File tools must register as sandboxed stubs, never run in the parent. */
static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    assert(tool_file_read_register(&reg, &file_ctx) == 0);
    assert(tool_file_write_register(&reg, &file_ctx) == 0);
    assert(tools_lookup(&reg, "file_read")->handler == tool_sandboxed_stub);
    assert(tools_lookup(&reg, "file_write")->handler == tool_sandboxed_stub);
    tools_free(&reg);
}

/* An existing non-empty file is refused without the overwrite flag. */
static void test_write_overwrite(void) {
    char *r = test_file_tool_run("file_write", "{\"path\":\"hello.txt\",\"content\":\"overwritten\"}", &file_ctx);
    assert(strstr(r, "error") != NULL && strstr(r, "overwrite") != NULL);
    free(r);
    r = test_file_tool_run("file_read", "{\"path\":\"hello.txt\"}", &file_ctx);
    assert(strstr(r, "overwritten") == NULL);
    free(r);

    r = test_file_tool_run("file_write", "{\"path\":\"hello.txt\",\"content\":\"overwritten\",\"overwrite\":true}", &file_ctx);
    assert(strstr(r, "wrote") != NULL);
    free(r);
    r = test_file_tool_run("file_read", "{\"path\":\"hello.txt\"}", &file_ctx);
    assert(strcmp(r, "overwritten") == 0);
    free(r);
}

static void test_edit_batch(void) {
    char *r = test_file_tool_run("file_write",
        "{\"path\":\"edit.txt\",\"content\":\"alpha\\nbeta\\ngamma\"}", &file_ctx);
    free(r);
    r = test_file_tool_run("file_edit",
        "{\"path\":\"edit.txt\",\"edits\":["
        "{\"oldText\":\"alpha\",\"newText\":\"AAA\"},"
        "{\"oldText\":\"gamma\",\"newText\":\"GGG\"}]}", &file_ctx);
    assert(strstr(r, "applied 2 edits") != NULL);
    free(r);
    r = test_file_tool_run("file_read", "{\"path\":\"edit.txt\"}", &file_ctx);
    assert(strcmp(r, "AAA\nbeta\nGGG") == 0);
    free(r);
}

static void test_edit_not_unique(void) {
    char *r = test_file_tool_run("file_write",
        "{\"path\":\"dup.txt\",\"content\":\"x x x\"}", &file_ctx);
    free(r);
    r = test_file_tool_run("file_edit",
        "{\"path\":\"dup.txt\",\"edits\":[{\"oldText\":\"x\",\"newText\":\"y\"}]}",
        &file_ctx);
    assert(strstr(r, "not unique") != NULL);
    free(r);
}

static void test_edit_not_found(void) {
    char *r = test_file_tool_run("file_edit",
        "{\"path\":\"hello.txt\",\"edits\":[{\"oldText\":\"zzz\",\"newText\":\"y\"}]}",
        &file_ctx);
    assert(strstr(r, "not found") != NULL);
    free(r);
}

static void test_edit_overlap(void) {
    char *r = test_file_tool_run("file_write",
        "{\"path\":\"ov.txt\",\"content\":\"abcdef\"}", &file_ctx);
    free(r);
    r = test_file_tool_run("file_edit",
        "{\"path\":\"ov.txt\",\"edits\":["
        "{\"oldText\":\"abcd\",\"newText\":\"X\"},"
        "{\"oldText\":\"cdef\",\"newText\":\"Y\"}]}", &file_ctx);
    assert(strstr(r, "overlapping") != NULL);
    free(r);
}

/* path_grant_hint: a sandboxed miss outside the grants says which grant to request. */
static FileReadCtx sandboxed_ctx(void) {
    FileReadCtx c = {0};
    c.workspace = tmpdir;
    c.sb.sandbox = 1;
    return c;
}

static void test_hint_outside_granted(void) {
    FileReadCtx hctx = sandboxed_ctx();
    char *r = test_file_tool_run("file_read", "{\"path\":\"/definitely/not/granted/file.txt\"}", &hctx);
    assert(strstr(r, "outside your granted areas") != NULL);
    assert(strstr(r, "request_config {\"changes\"") != NULL);
    assert(strstr(r, "\"read_paths\"") != NULL);
    free(r);
}

static void test_no_hint_inside_workspace(void) {
    FileReadCtx hctx = sandboxed_ctx();
    char args[512];
    snprintf(args, sizeof(args), "{\"path\":\"%s/missing.txt\"}", tmpdir);
    char *r = test_file_tool_run("file_read", args, &hctx);
    assert(strcmp(r, "error: cannot open file") == 0);
    free(r);
}

static void test_no_hint_when_unsandboxed(void) {
    FileReadCtx hctx2 = {0};
    hctx2.workspace = tmpdir;
    hctx2.sb.sandbox = 0;
    char *r = test_file_tool_run("file_read", "{\"path\":\"/definitely/not/granted/file.txt\"}", &hctx2);
    assert(strcmp(r, "error: cannot open file") == 0);
    free(r);
}

static void test_hint_read_path_grant(void) {
    FileReadCtx hctx = sandboxed_ctx();
    static char *rp[] = { "/granted/read" };
    hctx.sb.read_paths = rp;
    hctx.sb.read_path_count = 1;

    /* Inside granted read path — genuinely missing file, plain error */
    char *r = test_file_tool_run("file_read", "{\"path\":\"/granted/read/missing.txt\"}", &hctx);
    assert(strcmp(r, "error: cannot open file") == 0);
    free(r);

    /* Component-boundary mismatch — "/granted/readother" is NOT under "/granted/read" */
    r = test_file_tool_run("file_read", "{\"path\":\"/granted/readother/x.txt\"}", &hctx);
    assert(strstr(r, "outside your granted areas") != NULL);
    free(r);

}

static void test_hint_write_mode(void) {
    FileReadCtx hctx = sandboxed_ctx();

    /* Write to ungranted path — hint requests write_paths */
    char *r = test_file_tool_run("file_write", "{\"path\":\"/nope/out.txt\",\"content\":\"x\"}", &hctx);
    assert(strstr(r, "outside your granted areas") != NULL);
    assert(strstr(r, "\"write_paths\":[\"/nope\"]") != NULL);
    free(r);

    /* List ungranted dir — hint suggests the dir itself under read_paths */
    r = test_file_tool_run("file_list", "{\"path\":\"/nope\"}", &hctx);
    assert(strstr(r, "outside your granted areas") != NULL);
    assert(strstr(r, "\"read_paths\":[\"/nope\"]") != NULL);
    free(r);

}

int main(void) {
    TEST_INIT();
    printf("test_tool_file:\n");
    setup();
    RUN(test_smoke);
    RUN(test_register);
    RUN(test_write_overwrite);
    RUN(test_edit_batch);
    RUN(test_edit_not_unique);
    RUN(test_edit_not_found);
    RUN(test_edit_overlap);
    RUN(test_hint_outside_granted);
    RUN(test_no_hint_inside_workspace);
    RUN(test_no_hint_when_unsandboxed);
    RUN(test_hint_read_path_grant);
    RUN(test_hint_write_mode);
    cleanup();
    return 0;
}
