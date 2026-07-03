/* Coverage for the qjs_host_eval.c natives that reach the JS engine via
 * js_eval: fs.readFile/writeFile/readdir/stat/lstat/cwd, console.log
 * capture, and the prelude traps (require/process/module/Map/Set) that
 * turn missing Node globals into helpful TypeErrors instead of silent
 * undefined-is-not-a-function failures. Calls tool_js_eval_handler()
 * directly (host mode, no fork) — same entry point test_tool_js.c uses. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "tool_js.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

static const char *WORK_DIR = "/tmp/test_qjs_host_eval_work";

static void cleanup(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", WORK_DIR);
    system(cmd);
}

static void setup(void) {
    cleanup();
    char cmd[256];
    /* rd/ is dedicated to the readdir test — everything else writes loose
     * files straight into WORK_DIR, so readdir would otherwise see whatever
     * earlier tests left behind. */
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/rd/subdir", WORK_DIR);
    system(cmd);
}

/* Runs `code`, expects the result to start with "error:" and contain `needle`. */
static void expect_error(const char *name, const char *code, const char *needle) {
    tests_run++; printf("  %s... ", name);
    char args[512];
    snprintf(args, sizeof(args), "{\"code\":%s}", code);
    char *r = tool_js_eval_handler(args, NULL);
    if (!r || strncmp(r, "error:", 6) != 0 || !strstr(r, needle)) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

/* Runs `code`, expects the result to equal `want` exactly. */
static void expect_eq(const char *name, const char *code, const char *want) {
    tests_run++; printf("  %s... ", name);
    char args[1024];
    snprintf(args, sizeof(args), "{\"code\":%s}", code);
    char *r = tool_js_eval_handler(args, NULL);
    if (!r || strcmp(r, want) != 0) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

/* ── fs.* ─────────────────────────────────────────────────────────── */

static void test_fs_write_read_roundtrip(void) {
    char path[512], code[1600], want[64];
    snprintf(path, sizeof(path), "%s/hello.txt", WORK_DIR);
    snprintf(code, sizeof(code),
        "\"var w = fs.writeFile('%s', 'hello'); "
        "var r = fs.readFile('%s'); "
        "w[0] + '|' + w[1] + '|' + r[0] + '|' + r[1]\"", path, path);
    snprintf(want, sizeof(want), "5|0|hello|0");
    expect_eq("fs_write_read_roundtrip", code, want);
}

static void test_fs_read_missing(void) {
    /* ENOENT is 2 on every target this project ships to (Linux/glibc+musl). */
    expect_eq("fs_read_missing",
        "\"var r = fs.readFile('/tmp/test_qjs_host_eval_missing_xyz'); r[0] + '|' + r[1]\"",
        "null|2");
}

static void test_fs_readdir(void) {
    char rd[512], f1[600], f2[600], code[1024];
    snprintf(rd, sizeof(rd), "%s/rd", WORK_DIR);
    snprintf(f1, sizeof(f1), "%s/a.txt", rd);
    snprintf(f2, sizeof(f2), "%s/b.txt", rd);
    FILE *f = fopen(f1, "w"); fputs("x", f); fclose(f);
    f = fopen(f2, "w"); fputs("y", f); fclose(f);
    snprintf(code, sizeof(code),
        "\"var r = fs.readdir('%s'); "
        "var names = []; for (var i = 0; i < r[0].length; i++) names.push(r[0][i].name + ':' + r[0][i].type); "
        "names.sort(); names.join(',') + '|' + r[1]\"", rd);
    expect_eq("fs_readdir", code, "a.txt:file,b.txt:file,subdir:dir|0");
}

static void test_fs_readdir_missing(void) {
    expect_eq("fs_readdir_missing",
        "\"var r = fs.readdir('/tmp/test_qjs_host_eval_missing_dir'); r[0] + '|' + r[1]\"",
        "null|2");
}

static void test_fs_stat(void) {
    char path[512], code[768], want[64];
    snprintf(path, sizeof(path), "%s/hello.txt", WORK_DIR);
    FILE *f = fopen(path, "w"); fputs("hello", f); fclose(f);
    snprintf(code, sizeof(code),
        "\"var r = fs.stat('%s'); r[0].size + '|' + r[0].isDir + '|' + r[1]\"", path);
    snprintf(want, sizeof(want), "5|false|0");
    expect_eq("fs_stat_file", code, want);
}

static void test_fs_stat_dir(void) {
    char code[768];
    snprintf(code, sizeof(code),
        "\"var r = fs.stat('%s'); r[0].isDir + '|' + r[1]\"", WORK_DIR);
    expect_eq("fs_stat_dir", code, "true|0");
}

static void test_fs_lstat_symlink(void) {
    char target[512], link[512], code[768];
    snprintf(target, sizeof(target), "%s/hello.txt", WORK_DIR);
    snprintf(link, sizeof(link), "%s/hello.link", WORK_DIR);
    if (symlink(target, link) != 0) { TEST("fs_lstat_symlink"); FAIL("symlink() setup failed"); return; }
    snprintf(code, sizeof(code),
        "\"var r = fs.lstat('%s'); r[0].isLink + '|' + r[1]\"", link);
    expect_eq("fs_lstat_symlink", code, "true|0");
}

static void test_fs_cwd(void) {
    char old_cwd[512], cur_cwd[512], code[128], want[600];
    getcwd(old_cwd, sizeof(old_cwd));
    if (chdir(WORK_DIR) != 0) { TEST("fs_cwd"); FAIL("chdir setup failed"); return; }
    /* WORK_DIR may itself be a symlink (e.g. macOS /tmp) — compare against
     * what getcwd() resolves to, not the literal WORK_DIR string. */
    getcwd(cur_cwd, sizeof(cur_cwd));
    snprintf(code, sizeof(code), "\"var r = fs.cwd(); r[0] + '|' + r[1]\"");
    snprintf(want, sizeof(want), "%s|0", cur_cwd);
    expect_eq("fs_cwd", code, want);
    chdir(old_cwd);
}

/* ── console.log / print capture ─────────────────────────────────── */

static void test_console_log_single(void) {
    expect_eq("console_log_single", "\"console.log('hello')\"", "hello");
}

static void test_console_log_multi_args(void) {
    expect_eq("console_log_multi_args", "\"console.log('answer', 42)\"", "answer 42");
}

static void test_console_log_multiple_calls(void) {
    /* __console_buf.join('\n') in the driver uses a real newline, not "\\n". */
    expect_eq("console_log_multiple_calls",
        "\"console.log('one'); console.log('two')\"", "one\ntwo");
}

static void test_console_log_object(void) {
    expect_eq("console_log_object", "\"console.log({a:1})\"", "{\"a\":1}");
}

static void test_console_warn_error_alias(void) {
    expect_eq("console_warn_error_alias",
        "\"console.warn('w'); console.error('e')\"", "w\ne");
}

static void test_print_alias(void) {
    expect_eq("print_alias", "\"print('via print')\"", "via print");
}

/* ── Prelude traps: missing Node globals fail loud, not silently ───── */

static void test_prelude_require(void) {
    expect_error("prelude_require", "\"require()\"", "require() not available");
}

static void test_prelude_process_env(void) {
    expect_error("prelude_process_env", "\"process.env\"", "process.env not available");
}

static void test_prelude_process_cwd(void) {
    expect_error("prelude_process_cwd", "\"process.cwd\"", "process.cwd not available");
}

static void test_prelude_process_argv(void) {
    expect_error("prelude_process_argv", "\"process.argv\"", "process.argv not available");
}

static void test_prelude_process_exit(void) {
    expect_error("prelude_process_exit", "\"process.exit\"", "process.exit not available");
}

static void test_prelude_process_platform(void) {
    expect_error("prelude_process_platform", "\"process.platform\"", "process.platform not available");
}

static void test_prelude_module_exports_get(void) {
    expect_error("prelude_module_exports_get", "\"module.exports\"", "module.exports not available");
}

static void test_prelude_module_exports_set(void) {
    expect_error("prelude_module_exports_set", "\"module.exports = 1\"", "module.exports not available");
}

static void test_prelude_map(void) {
    expect_error("prelude_map", "\"new Map()\"", "Map not available");
}

static void test_prelude_set(void) {
    expect_error("prelude_set", "\"new Set()\"", "Set not available");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_qjs_host_eval:\n");
    setup();

    test_fs_write_read_roundtrip();
    test_fs_read_missing();
    test_fs_readdir();
    test_fs_readdir_missing();
    test_fs_stat();
    test_fs_stat_dir();
    test_fs_lstat_symlink();
    test_fs_cwd();

    test_console_log_single();
    test_console_log_multi_args();
    test_console_log_multiple_calls();
    test_console_log_object();
    test_console_warn_error_alias();
    test_print_alias();

    test_prelude_require();
    test_prelude_process_env();
    test_prelude_process_cwd();
    test_prelude_process_argv();
    test_prelude_process_exit();
    test_prelude_process_platform();
    test_prelude_module_exports_get();
    test_prelude_module_exports_set();
    test_prelude_map();
    test_prelude_set();

    cleanup();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
