/* web_fetch is the shipped web-fetch extension: its handler (the embedded
 * fetch.qjs, exactly what installs) runs in the js leaf in-process, host
 * mode, against the loopback mock. The C pieces it stands on — the
 * HTML-to-markdown parser and http_request's denied-host hint — get their
 * own tables. */
#define _POSIX_C_SOURCE 200809L
#include "test_util.h"
#include "mock_server.h"
#include "js_http_fetch.h"
#include "qjs_html.h"
#include "templates.h"
#include "tool_args.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define HANDLER_DIR "/tmp/cclaw_test_web_fetch"
#define HANDLER     HANDLER_DIR "/fetch.qjs"

static char *run_web_fetch(const char *args, char **rules, size_t n, int sandbox,
                           int *is_error) {
    struct RunToolParam p[2] = {
        {.key = "filename", .kind = TOOL_ARG_TEXT, .value = HANDLER},
        {.key = "args", .kind = TOOL_ARG_JSON, .value = (char *)args},
    };
    RunToolParsed q;
    memset(&q, 0, sizeof(q));
    q.vehicle = EXEC_JS;
    q.tool_name = "web_fetch";
    q.params = p;
    q.param_count = 2;
    q.host_rules = rules;
    q.host_count = n;
    q.plan.ne = sandbox ? NE_NETNS : NE_NONE;
    *is_error = 0;
    return tool_js_child_run(&q, is_error);
}

/* Untrusted page bytes: what survives conversion, and malformed markup that
 * must come back as text rather than crash or run off the end. */
static const struct { const char *html; const char *keep[2]; const char *drop; } strips[] = {
    {"<html><body><p>Hello <b>world</b></p></body></html>", {"Hello", "world"}, "<"},
    {"<p>before</p><script>var x=1;</script><p>after</p>", {"before", "after"}, "var"},
    {"<style>.x{color:red}</style><p>text</p>", {"text", NULL}, "color"},
    {"<h2>Title</h2><a href='/x'>link</a>", {"## Title", "[link](/x)"}, "<a"},
    {"<p>open tag never closes <a href=\"x", {"open tag never closes", NULL}, NULL},
    {"<a href=\"u\">dangling link", {"[dangling link", NULL}, NULL},
    {"fish &amp chips &lt;", {"fish &amp chips", "<"}, NULL},
    {"<script>never ends", {NULL, NULL}, "never"},
};

static void test_html_to_markdown_table(void) {
    for (size_t i = 0; i < sizeof(strips) / sizeof(strips[0]); i++) {
        char *out = html_to_markdown(strips[i].html, strlen(strips[i].html));
        assert(out);
        int ok = !strips[i].drop || strstr(out, strips[i].drop) == NULL;
        for (int k = 0; k < 2 && strips[i].keep[k]; k++)
            ok = ok && strstr(out, strips[i].keep[k]) != NULL;
        if (!ok) { printf("    row %zu: \"%s\"\n", i, out); assert(0); }
        free(out);
    }
}

/* The denied-host hint: names the bare host (no port, no userinfo) and the
 * request_config route; NULL when the host is allowed or the URL has no
 * parseable host. rule NULL = no rules at all. */
static const struct {
    const char *rule; const char *url; const char *named; const char *absent;
} hints[] = {
    {"api.example.com", "https://api.example.com/v1/x", NULL, NULL},
    {"api.example.com", "https://evil.example.com/x", "evil.example.com", NULL},
    {".github.com", "https://api.github.com/repos", NULL, NULL},
    {".github.com", "https://github.com/", NULL, NULL},
    {".github.com", "https://evilgithub.com/", "evilgithub.com", NULL},
    {"h.example.com", "http://user:pw@h.example.com:8080/path?q=1", NULL, NULL},
    {"h.example.com", "http://u@x.example.com:99/p", "x.example.com", ":99"},
    {"h.example.com", "http://u@x.example.com:99/p", "x.example.com", "u@"},
    {"a.example.com", "not-a-url", NULL, NULL},
    {"a.example.com", "http://[::1/x", NULL, NULL},
    {NULL, "https://a.example.com/", "a.example.com", NULL},
};

static void test_host_hint_table(void) {
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        char *rules[1] = {(char *)hints[i].rule};
        char *h = js_http_denied_hint(hints[i].url, hints[i].rule ? rules : NULL,
                                      hints[i].rule ? 1 : 0);
        int ok;
        if (!hints[i].named) {
            ok = h == NULL;
        } else {
            char want[300];
            snprintf(want, sizeof(want), "host '%s'", hints[i].named);
            ok = h && strstr(h, want) && strstr(h, "request_config") &&
                 strstr(h, "\"hosts\"") &&
                 (!hints[i].absent || !strstr(h, hints[i].absent));
        }
        if (!ok) { printf("    row %zu: %s -> %s\n", i, hints[i].url, h ? h : "(null)"); assert(0); }
        free(h);
    }
}

/* The handler end to end. The empty-challenge retry is the 2026-07-06 ESPN
 * fix (a 202 with no body to the browser UA, the page to a plain one); the
 * capped row is a page over the 2 MB fetch limit, which must still fit the
 * engine's default heap. */
static void test_handler_table(void) {
    static const char *HTML_CT[] = {"Content-Type: text/html; charset=utf-8", NULL};
    size_t big = 2500 * 1024;
    char *huge = malloc(big + 1);
    assert(huge);
    memset(huge, 'a', big);
    huge[big] = '\0';

    struct {
        int s1; const char *b1; int s2; const char *b2; const char **hdrs;
        const char *args_extra; int err; const char *want; const char *absent;
        int requests;
    } rows[] = {
        {202, "", 200, "<p>ranking Argentina one</p>", NULL, "", 0,
         "ranking Argentina one", NULL, 2},
        {200, "<p>full content here</p>", 0, NULL, NULL, "", 0,
         "full content here", NULL, 1},
        {200, "<h1>Title</h1><p>body</p>", 0, NULL, HTML_CT, "", 0,
         "[total=12]\n# Title\nbody", "<h1>", 1},
        {200, "<h1>Title</h1>", 0, NULL, HTML_CT, ",\"raw\":true", 0,
         "<h1>Title</h1>", NULL, 1},
        {200, "{\"a\":\"<b>x</b>\"}", 0, NULL, NULL, "", 0,
         "{\"a\":\"<b>x</b>\"}", NULL, 1},
        {404, "<p>gone</p>", 0, NULL, NULL, "", 1, "HTTP 404", "gone", 1},
        {200, huge, 0, NULL, NULL, "", 0,
         "[total=2097152 capped(source exceeded fetch limit)]", NULL, 1},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int port = mock_server_start();
        assert(port > 0);
        mock_server_enqueue_with_headers(rows[i].s1, rows[i].b1, rows[i].hdrs);
        if (rows[i].b2) mock_server_enqueue_with_headers(rows[i].s2, rows[i].b2,
                                                         rows[i].hdrs);
        char args[256];
        snprintf(args, sizeof(args),
                 "{\"url\":\"http://127.0.0.1:%d/v1/chat/completions\"%s}",
                 port, rows[i].args_extra);
        int is_error = 0;
        char *r = run_web_fetch(args, NULL, 0, 0, &is_error);
        int ok = r && is_error == rows[i].err && strstr(r, rows[i].want) &&
                 (!rows[i].absent || !strstr(r, rows[i].absent)) &&
                 mock_server_request_count() == rows[i].requests;
        if (!ok) {
            printf("    row %zu: err=%d requests=%d \"%.200s\"\n", i, is_error,
                   mock_server_request_count(), r ? r : "(null)");
            assert(0);
        }
        free(r);
        mock_server_stop();
    }
    free(huge);
}

/* What reaches the model when the call cannot run: an ungranted host under
 * enforcement names the grant (the refused connection stands in for the
 * proxy's refusal); with no enforcement it is a plain failure; a URL that is
 * not http(s) never leaves the handler. */
static void test_handler_refusals(void) {
    char *rules[1] = {"example.com"};
    struct {
        const char *args; char **rules; size_t n; int sandbox; int err;
        const char *want; const char *absent;
    } rows[] = {
        {"{\"url\":\"http://127.0.0.1:1/\"}", rules, 1, 1, 0,
         "error: host '127.0.0.1' is not in your allowed hosts", NULL},
        {"{\"url\":\"http://127.0.0.1:1/\"}", rules, 1, 0, 1,
         "HTTP request failed", "allowed hosts"},
        {"{\"url\":\"ftp://bad\"}", NULL, 0, 0, 1, "http://", NULL},
        {"{}", NULL, 0, 0, 1, "url", NULL},
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int is_error = 0;
        char *r = run_web_fetch(rows[i].args, rows[i].rules, rows[i].n,
                                rows[i].sandbox, &is_error);
        int ok = r && is_error == rows[i].err && strstr(r, rows[i].want) &&
                 (!rows[i].absent || !strstr(r, rows[i].absent));
        if (!ok) { printf("    row %zu: err=%d \"%s\"\n", i, is_error, r ? r : "(null)"); assert(0); }
        free(r);
    }
}

int main(void) {
    TEST_INIT();
    printf("test_tool_web_fetch:\n");
    mkdir(HANDLER_DIR, 0755);
    FILE *f = fopen(HANDLER, "w");
    assert(f && fputs(TPL_EXT_WEB_FETCH_FETCH_QJS, f) >= 0 && fclose(f) == 0);
    unsetenv("CCLAW_JS_HEAP_MB");
    RUN(test_html_to_markdown_table);
    RUN(test_host_hint_table);
    RUN(test_handler_table);
    RUN(test_handler_refusals);
    unlink(HANDLER);
    rmdir(HANDLER_DIR);
    return 0;
}
