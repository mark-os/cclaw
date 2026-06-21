/* T254/T255: Test extension discovery and loading */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include "extension.h"
#include "tool_js.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  %s... ", #name); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)

static void mkdirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(content, f); fclose(f); }
}

static void test_discover_empty(void) {
    TEST(discover_empty);
    size_t count = 99;
    char **paths = extension_discover("/tmp/cclaw_ext_test_nonexist", &count);
    if (count != 0) FAIL("expected 0");
    extension_list_free(paths, count);
    PASS();
}

static void test_discover_files(void) {
    TEST(discover_files);
    const char *ws = "/tmp/cclaw_ext_test_ws";
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    /* Create some .mjs files */
    char p1[512], p2[512], p3[512];
    snprintf(p1, sizeof(p1), "%s/beta.mjs", ext_dir);
    snprintf(p2, sizeof(p2), "%s/alpha.mjs", ext_dir);
    snprintf(p3, sizeof(p3), "%s/readme.txt", ext_dir); /* not .mjs */
    write_file(p1, "// beta");
    write_file(p2, "// alpha");
    write_file(p3, "not js");

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    if (count != 2) { printf("got %zu ", count); FAIL("expected 2 files"); }
    /* Should be sorted alphabetically */
    if (!strstr(paths[0], "alpha.mjs")) FAIL("first should be alpha.mjs");
    if (!strstr(paths[1], "beta.mjs")) FAIL("second should be beta.mjs");
    extension_list_free(paths, count);

    unlink(p1); unlink(p2); unlink(p3);
    rmdir(ext_dir);
    char ws2[256]; snprintf(ws2, sizeof(ws2), "%s", ws); rmdir(ws2);
    PASS();
}

static void test_discover_subdir_index(void) {
    TEST(discover_subdir_index);
    const char *ws = "/tmp/cclaw_ext_test_ws2";
    char ext_dir[256], sub_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    snprintf(sub_dir, sizeof(sub_dir), "%s/extensions/myplugin", ws);
    mkdirs(sub_dir);

    char idx[512];
    snprintf(idx, sizeof(idx), "%s/index.mjs", sub_dir);
    write_file(idx, "// plugin");

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    if (count != 1) { printf("got %zu ", count); FAIL("expected 1"); }
    if (!strstr(paths[0], "myplugin/index.mjs")) FAIL("should be myplugin/index.mjs");
    extension_list_free(paths, count);

    unlink(idx); rmdir(sub_dir); rmdir(ext_dir);
    char ws2[256]; snprintf(ws2, sizeof(ws2), "%s", ws); rmdir(ws2);
    PASS();
}

static void test_load_success(void) {
    TEST(load_success);
    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("js_runtime_create");

    const char *ws = "/tmp/cclaw_ext_test_ws3";
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512];
    snprintf(p1, sizeof(p1), "%s/good.mjs", ext_dir);
    write_file(p1, "globalThis.__ext_loaded = true;");

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    if (count != 1) { js_runtime_destroy(rt); FAIL("discover failed"); }

    Config cfg = {.log_level = LOG_LEVEL_DEBUG};
    int loaded = extension_load(paths, count, rt, NULL, &cfg, NULL, NULL);
    extension_list_free(paths, count);

    if (loaded != 1) { js_runtime_destroy(rt); FAIL("expected 1 loaded"); }

    js_runtime_destroy(rt);
    unlink(p1); rmdir(ext_dir);
    char ws2[256]; snprintf(ws2, sizeof(ws2), "%s", ws); rmdir(ws2);
    PASS();
}

static void test_load_throw_skips(void) {
    TEST(load_throw_skips);
    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) FAIL("js_runtime_create");

    const char *ws = "/tmp/cclaw_ext_test_ws4";
    char ext_dir[256];
    snprintf(ext_dir, sizeof(ext_dir), "%s/extensions", ws);
    mkdirs(ext_dir);

    char p1[512], p2[512];
    snprintf(p1, sizeof(p1), "%s/bad.mjs", ext_dir);
    snprintf(p2, sizeof(p2), "%s/good.mjs", ext_dir);
    write_file(p1, "throw new Error('intentional');");
    write_file(p2, "globalThis.__ext2 = 1;");

    size_t count = 0;
    char **paths = extension_discover(ws, &count);
    if (count != 2) { js_runtime_destroy(rt); FAIL("discover"); }

    Config cfg = {.log_level = LOG_LEVEL_DEBUG};
    int loaded = extension_load(paths, count, rt, NULL, &cfg, NULL, NULL);
    extension_list_free(paths, count);

    /* bad.mjs throws → skipped, good.mjs succeeds */
    if (loaded != 1) {
        printf("loaded=%d ", loaded);
        js_runtime_destroy(rt);
        FAIL("expected 1 loaded (bad skipped)");
    }

    js_runtime_destroy(rt);
    unlink(p1); unlink(p2); rmdir(ext_dir);
    char ws2[256]; snprintf(ws2, sizeof(ws2), "%s", ws); rmdir(ws2);
    PASS();
}

int main(void) {
    printf("test_extension:\n");
    test_discover_empty();
    test_discover_files();
    test_discover_subdir_index();
    test_load_success();
    test_load_throw_skips();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
