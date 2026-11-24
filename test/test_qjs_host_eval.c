/* Coverage for the qjs_host_eval.c natives that reach the JS engine via
 * js_eval: fs.readFile/writeFile/readdir/stat/lstat/cwd, console.log
 * capture, and the prelude traps (require/process/module) that turn missing
 * Node globals into helpful TypeErrors instead of silent
 * undefined-is-not-a-function failures. Runs the JS tier in-process (host
 * mode, no fork), the same entry point test_tool_js.c uses. */
#define _GNU_SOURCE
#include "test_util.h"
#include <assert.h>

static const char *WORK_DIR = "/tmp/test_qjs_host_eval_work";

static void cleanup(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", WORK_DIR);
    if (system(cmd) != 0) { /* best effort */ }
}

static void setup(void) {
    cleanup();
    char cmd[256];
    /* rd/ is dedicated to the readdir test, so it sees only its own files. */
    snprintf(cmd, sizeof(cmd), "mkdir -p %s/rd/subdir", WORK_DIR);
    assert(system(cmd) == 0);
}

/* `code` is a JSON string literal. want_error: the result must start with
 * "error:" and contain `want`; otherwise it must equal `want`. */
static void expect(const char *code, const char *want, int want_error) {
    char args[1600];
    snprintf(args, sizeof(args), "{\"code\":%s}", code);
    char *r = test_js_eval_run_json(args);
    int ok = r && (want_error ? strncmp(r, "error:", 6) == 0 && strstr(r, want)
                              : strcmp(r, want) == 0);
    if (!ok) fprintf(stderr, "code %s: got %s, want %s\n", code, r ? r : "(null)", want);
    assert(ok);
    free(r);
}

static void write_file(const char *path, const char *s) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(s, f);
    fclose(f);
}

static void test_fs(void) {
    char path[512], link[512], code[1400], cwd[512], old_cwd[512];
    snprintf(path, sizeof(path), "%s/hello.txt", WORK_DIR);
    snprintf(code, sizeof(code),
        "\"var w = fs.writeFile('%s', 'hello'); var r = fs.readFile('%s'); w + '|' + r\"",
        path, path);
    expect(code, "5|hello", 0);
    expect("\"fs.readFile('/tmp/test_qjs_host_eval_missing_xyz')\"", "No such file", 1);

    snprintf(code, sizeof(code), "\"var r = fs.stat('%s'); r.size + '|' + r.isDir\"", path);
    expect(code, "5|false", 0);
    snprintf(code, sizeof(code), "\"fs.stat('%s').isDir\"", WORK_DIR);
    expect(code, "true", 0);

    snprintf(link, sizeof(link), "%s/hello.link", WORK_DIR);
    assert(symlink(path, link) == 0);
    snprintf(code, sizeof(code), "\"fs.lstat('%s').isLink\"", link);
    expect(code, "true", 0);

    char f[600];
    snprintf(f, sizeof(f), "%s/rd/a.txt", WORK_DIR);
    write_file(f, "x");
    snprintf(f, sizeof(f), "%s/rd/b.txt", WORK_DIR);
    write_file(f, "y");
    snprintf(code, sizeof(code),
        "\"var r = fs.readdir('%s/rd'); var names = []; "
        "for (var i = 0; i < r.length; i++) names.push(r[i].name + ':' + r[i].type); "
        "names.sort(); names.join(',')\"", WORK_DIR);
    expect(code, "a.txt:file,b.txt:file,subdir:dir", 0);
    expect("\"fs.readdir('/tmp/test_qjs_host_eval_missing_dir')\"", "No such file", 1);

    /* WORK_DIR may itself be a symlink (e.g. macOS /tmp): compare against
     * what getcwd() resolves to. */
    assert(getcwd(old_cwd, sizeof(old_cwd)));
    assert(chdir(WORK_DIR) == 0);
    assert(getcwd(cwd, sizeof(cwd)));
    expect("\"fs.cwd()\"", cwd, 0);
    assert(chdir(old_cwd) == 0);
}

/* ~500KB generated JSON string: would OOM on the old 1MB heap. */
static void test_large_json_parse(void) {
    expect("\"var s = '['; for(var i=0;i<10000;i++) s += '{\\\"x\\\":'+i+'},'; "
           "s = s.slice(0,-1) + ']'; JSON.parse(s).length\"", "10000", 0);
}

static void test_console_capture(void) {
    static const struct { const char *code, *want; } cases[] = {
        { "\"console.log('answer', 42)\"", "answer 42" },
        /* The driver joins with a real newline, not "\\n". */
        { "\"console.log('one'); console.log('two')\"", "one\ntwo" },
        { "\"console.log({a:1})\"", "{\"a\":1}" },
        { "\"console.warn('w'); console.error('e')\"", "w\ne" },
        { "\"print('via print')\"", "via print" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        expect(cases[i].code, cases[i].want, 0);
}

/* Missing Node globals fail loud, naming what is missing. */
static void test_prelude_traps(void) {
    static const struct { const char *code, *want; } cases[] = {
        { "\"require()\"", "require()" },
        { "\"process.env\"", "process.env" },
        { "\"process.cwd\"", "process.cwd" },
        { "\"process.argv\"", "process.argv" },
        { "\"process.exit\"", "process.exit" },
        { "\"process.platform\"", "process.platform" },
        { "\"module.exports\"", "module.exports" },
        { "\"module.exports = 1\"", "module.exports" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        expect(cases[i].code, cases[i].want, 1);
        expect(cases[i].code, "not available", 1);
    }
}

/* Heap cap: env → the limit the eval actually gets. Overflow the heap by a
 * wide margin so the OOM is decisive wherever it lands, then read the limit
 * back out of the hint. The hint has to quote the live cap: a stale number
 * tells the agent to shrink input that would have fit, or to retry input
 * that never will. Each row runs the heap dry, so keep the caps small; the
 * 512MB upper clamp is left to review rather than spending 15s filling it. */
static void test_heap_hint(void) {
    static const struct { const char *env, *want; } cases[] = {
        { NULL, "limit is 8MB" },
        { "2", "limit is 2MB" },
        { "nonsense", "limit is 8MB" },
        { "0", "limit is 8MB" },   /* clamped low */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].env) setenv("CCLAW_JS_HEAP_MB", cases[i].env, 1);
        else unsetenv("CCLAW_JS_HEAP_MB");
        /* 1MB per iteration: overflows any cap in a few hundred steps, and
         * stays far under both the instruction limit and QuickJS's max string
         * length (which a doubling string hits first, "string too long"). */
        char *r = test_js_eval_run_json(
            "{\"code\":\"var a=[]; for(;;) a.push(new Array(1000000).join('x')); a.length\"}");
        int ok = r && strstr(r, "out of memory") && strstr(r, cases[i].want);
        if (!ok) fprintf(stderr, "row %zu (env %s): %s\n", i,
                         cases[i].env ? cases[i].env : "unset", r ? r : "(null)");
        assert(ok);
        free(r);
    }
    unsetenv("CCLAW_JS_HEAP_MB");
}

int main(void) {
    TEST_INIT();
    setup();
    RUN(test_fs);
    RUN(test_large_json_parse);
    RUN(test_console_capture);
    RUN(test_prelude_traps);
    RUN(test_heap_hint);
    cleanup();
    return 0;
}
