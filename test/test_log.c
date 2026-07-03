/* Test: log level gating, trace=1 marking, ctx suffix, heap-promote path.
 * cclaw_log_init(1) sets LOG_PERROR, so every syslog() write is teed to
 * stderr; we dup2 stderr onto a temp file and assert against its contents. */
#define _POSIX_C_SOURCE 200809L
#include "log.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_logfile[] = "/tmp/cclaw_test_log_XXXXXX";

/* Read the whole captured stderr file into a malloc'd string. */
static char *read_log(void) {
    FILE *f = fopen(g_logfile, "r");
    assert(f);
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)len + 1);
    assert(buf);
    size_t got = fread(buf, 1, (size_t)len, f);
    buf[got] = '\0';
    fclose(f);
    return buf;
}

/* Return the line (NUL-terminated, malloc'd) containing marker, or NULL. */
static char *find_line(const char *marker) {
    char *all = read_log();
    char *hit = strstr(all, marker);
    if (!hit) { free(all); return NULL; }
    char *start = hit;
    while (start > all && start[-1] != '\n') start--;
    char *end = strchr(hit, '\n');
    if (!end) end = hit + strlen(hit);
    char *line = strndup(start, (size_t)(end - start));
    free(all);
    return line;
}

static void test_level_gating(void) {
    cclaw_log_set_level(LOG_LEVEL_INFO);
    assert(cclaw_log_level() == LOG_LEVEL_INFO);
    LOG_DEBUG_("gated_debug_marker");
    LOG_INFO_("passed_info_marker");
    LOG_WARN_("passed_warn_marker");
    LOG_ERROR_("passed_error_marker");
    char *all = read_log();
    assert(!strstr(all, "gated_debug_marker"));
    assert(strstr(all, "passed_info_marker"));
    assert(strstr(all, "passed_warn_marker"));
    assert(strstr(all, "passed_error_marker"));
    free(all);

    cclaw_log_set_level(LOG_LEVEL_ERROR);
    LOG_WARN_("gated_warn_marker");
    LOG_INFO_("gated_info_marker");
    all = read_log();
    assert(!strstr(all, "gated_warn_marker"));
    assert(!strstr(all, "gated_info_marker"));
    free(all);
    printf("  level_gating... PASS\n");
}

static void test_trace_marking(void) {
    cclaw_log_set_level(LOG_LEVEL_TRACE);
    LOG_TRACE_("trace_marker payload");
    LOG_DEBUG_("debug_marker payload");
    char *line = find_line("trace_marker");
    assert(line && strstr(line, " trace=1"));
    free(line);
    line = find_line("debug_marker");
    assert(line && !strstr(line, "trace=1"));
    free(line);
    printf("  trace_marking... PASS\n");
}

static void test_ctx_suffix(void) {
    cclaw_log_set_level(LOG_LEVEL_INFO);
    cclaw_log_set_ctx(42, 7, "tester");
    LOG_INFO_("ctx_marker");
    char *line = find_line("ctx_marker");
    assert(line);
    assert(strstr(line, "session=42"));
    assert(strstr(line, "turn=7"));
    assert(strstr(line, "agent=\"tester\""));
    free(line);

    cclaw_log_clear_ctx();
    LOG_INFO_("noctx_marker");
    line = find_line("noctx_marker");
    assert(line && !strstr(line, "session="));
    free(line);
    printf("  ctx_suffix... PASS\n");
}

static void test_heap_promote(void) {
    /* >1024 bytes forces the heap path in cclaw_log_write; the message must
     * arrive whole, not truncated at the stack buffer. */
    cclaw_log_set_level(LOG_LEVEL_INFO);
    size_t n = 3000;
    char *big = malloc(n + 1);
    memset(big, 'A', n);
    big[n] = '\0';
    LOG_INFO_("bigmsg_marker %s end_marker", big);
    char *line = find_line("bigmsg_marker");
    assert(line);
    char *run = strchr(line, 'A');
    assert(run);
    size_t seen = strspn(run, "A");
    assert(seen == n);
    assert(strstr(line, "end_marker"));
    free(line);
    free(big);
    printf("  heap_promote... PASS\n");
}

static void test_parse_and_name(void) {
    assert(log_level_parse("trace") == LOG_LEVEL_TRACE);
    assert(log_level_parse("debug") == LOG_LEVEL_DEBUG);
    assert(log_level_parse("info")  == LOG_LEVEL_INFO);
    assert(log_level_parse("warn")  == LOG_LEVEL_WARN);
    assert(log_level_parse("error") == LOG_LEVEL_ERROR);
    assert(log_level_parse(NULL)    == LOG_LEVEL_INFO);
    assert(log_level_parse("bogus") == LOG_LEVEL_INFO);
    assert(strcmp(log_level_name(LOG_LEVEL_WARN), "warn") == 0);
    assert(strcmp(log_level_name(LOG_LEVEL_TRACE), "trace") == 0);
    /* Round-trip every level through name → parse. */
    for (LogLevel l = LOG_LEVEL_ERROR; l <= LOG_LEVEL_TRACE; l++)
        assert(log_level_parse(log_level_name(l)) == l);
    printf("  parse_and_name... PASS\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_log:\n");

    int fd = mkstemp(g_logfile);
    assert(fd >= 0);
    assert(dup2(fd, STDERR_FILENO) == STDERR_FILENO);
    close(fd);

    cclaw_log_init(1);  /* LOG_PERROR tees every write to (captured) stderr */

    test_level_gating();
    test_trace_marking();
    test_ctx_suffix();
    test_heap_promote();
    test_parse_and_name();

    unlink(g_logfile);
    printf("all log tests passed\n");
    return 0;
}
