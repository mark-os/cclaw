#define _POSIX_C_SOURCE 200809L
#include "test_util.h"
#include "mock_server.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static const struct { const char *html; const char *keep[2]; const char *drop; } strips[] = {
    {"<html><body><p>Hello <b>world</b></p></body></html>", {"Hello", "world"}, "<"},
    {"<p>before</p><script>var x=1;</script><p>after</p>", {"before", "after"}, "var"},
    {"<style>.x{color:red}</style><p>text</p>", {"text", NULL}, "color"},
};

static void test_html_strip_table(void) {
    for (size_t i = 0; i < sizeof(strips) / sizeof(strips[0]); i++) {
        char *out = html_to_markdown(strips[i].html, strlen(strips[i].html));
        assert(out);
        int ok = strstr(out, strips[i].drop) == NULL;
        for (int k = 0; k < 2 && strips[i].keep[k]; k++)
            ok = ok && strstr(out, strips[i].keep[k]) != NULL;
        if (!ok) { printf("    row %zu: \"%s\"\n", i, out); assert(0); }
        free(out);
    }
}

static void test_handler_bad_url(void) {
    static const char *args[] = {"{\"url\":\"ftp://bad\"}", "{}"};
    for (size_t i = 0; i < 2; i++) {
        char *result = test_web_fetch_run(args[i], NULL);
        assert(strstr(result, "error") != NULL);
        free(result);
    }
}

static void test_register(void) {
    ToolRegistry reg;
    tools_init(&reg);
    int rc = tool_web_fetch_register(&reg, NULL);
    assert(rc == 0);
    ToolEntry *e = tools_lookup(&reg, "web_fetch");
    assert(e != NULL);
    assert(strcmp(e->name, "web_fetch") == 0);
    assert(e->recipe.vehicle == EXEC_SANDBOX && e->recipe.tier == SBX_WEB);
    tools_free(&reg);
}

/* The denied-host hint: names the bare host (no port, no userinfo) and the
 * request_config route; NULL when the host is allowed, under host mode, or
 * when the URL has no parseable host. rule NULL = no rules at all. */
static const struct {
    const char *rule; const char *url; int host_mode; const char *named; const char *absent;
} hints[] = {
    {"api.example.com", "https://api.example.com/v1/x", 0, NULL, NULL},
    {"api.example.com", "https://evil.example.com/x", 0, "evil.example.com", NULL},
    {"api.example.com", "https://evil.example.com/x", 1, NULL, NULL},
    {".github.com", "https://api.github.com/repos", 0, NULL, NULL},
    {".github.com", "https://github.com/", 0, NULL, NULL},
    {".github.com", "https://evilgithub.com/", 0, "evilgithub.com", NULL},
    {"h.example.com", "http://user:pw@h.example.com:8080/path?q=1", 0, NULL, NULL},
    {"h.example.com", "http://u@x.example.com:99/p", 0, "x.example.com", ":99"},
    {"h.example.com", "http://u@x.example.com:99/p", 0, "x.example.com", "u@"},
    {"a.example.com", "not-a-url", 0, NULL, NULL},
    {NULL, "https://a.example.com/", 0, "a.example.com", NULL},
};

static void test_host_hint_table(void) {
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        char *rules[1] = {(char *)hints[i].rule};
        char *h = web_fetch_host_hint(hints[i].url, hints[i].rule ? rules : NULL,
                                      hints[i].rule ? 1 : 0, hints[i].host_mode);
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

/* A truncated fetch writes the full document under workspace/.tool_results
 * and points at it in the result. */
static void test_returns_whole_document(void) {
    /* web_fetch no longer truncates or keeps its own copy: it returns the
     * document whole, and the broker spills + truncates on the way out, the
     * same path every tool takes (see test_spill_child). Keeping a second
     * mechanism here meant two truncation rules and two "full copy" homes. */
    int port = mock_server_start();
    assert(port > 0);

    size_t big = 30000;  /* would have been cut at the old 20000 default */
    char *body = malloc(big + 1);
    assert(body);
    memset(body, 'a', big);
    body[big] = '\0';
    mock_server_enqueue(200, body);
    free(body);

    char ws[] = "/tmp/cclaw_wf_ws_XXXXXX";
    assert(mkdtemp(ws) != NULL);

    WebFetchCtx ctx = {0};
    ctx.workspace = ws;
    ctx.host_mode = 1;  /* no egress enforcement in-test */

    char args[256];
    snprintf(args, sizeof(args),
             "{\"url\":\"http://127.0.0.1:%d/v1/chat/completions\"}", port);
    char *result = test_web_fetch_run(args, &ctx);
    assert(result);
    assert(strlen(result) >= big);                    /* nothing cut here */
    assert(strstr(result, "truncated") == NULL);
    assert(strstr(result, "saved to ") == NULL);
    assert(strstr(result, "[total=") != NULL);

    /* and no private stash left behind */
    char rdir[600];
    snprintf(rdir, sizeof(rdir), "%s/.tool_results", ws);
    assert(opendir(rdir) == NULL);

    free(result);
    rmdir(ws);
    mock_server_stop();
}

/* An empty challenge shell (202, no body) triggers one retry with the plain
 * UA, and the retry's real content is what gets returned. */
static void test_retry_on_empty_challenge(void) {
    int port = mock_server_start();
    assert(port > 0);
    mock_server_enqueue(202, "");                              /* challenge */
    mock_server_enqueue(200, "<p>ranking Argentina one</p>");  /* real page */

    WebFetchCtx ctx = {0};
    ctx.host_mode = 1;
    char args[256];
    snprintf(args, sizeof(args),
             "{\"url\":\"http://127.0.0.1:%d/v1/chat/completions\"}", port);
    char *r = test_web_fetch_run(args, &ctx);
    assert(r);
    assert(strstr(r, "ranking Argentina one") != NULL);  /* retry's body won */
    assert(mock_server_request_count() == 2);            /* exactly one retry */
    free(r);
    mock_server_stop();
}

/* A non-empty first response is used as-is — no second request. */
static void test_no_retry_when_body_present(void) {
    int port = mock_server_start();
    assert(port > 0);
    mock_server_enqueue(200, "<p>full content here</p>");

    WebFetchCtx ctx = {0};
    ctx.host_mode = 1;
    char args[256];
    snprintf(args, sizeof(args),
             "{\"url\":\"http://127.0.0.1:%d/v1/chat/completions\"}", port);
    char *r = test_web_fetch_run(args, &ctx);
    assert(r);
    assert(strstr(r, "full content here") != NULL);
    assert(mock_server_request_count() == 1);  /* no retry */
    free(r);
    mock_server_stop();
}

int main(void) {
    TEST_INIT();
    printf("test_tool_web_fetch:\n");
    RUN(test_returns_whole_document);
    RUN(test_retry_on_empty_challenge);
    RUN(test_no_retry_when_body_present);
    RUN(test_html_strip_table);
    RUN(test_handler_bad_url);
    RUN(test_register);
    RUN(test_host_hint_table);
    return 0;
}
