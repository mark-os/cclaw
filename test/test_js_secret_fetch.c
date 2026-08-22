/* Fetch-boundary {{SECRET:name}} resolution in the JS tier (qjs_host_eval.c):
 * placeholders in url/body/headers — including ones read from a .qjs file
 * body, the case the parent's arg interpolation can never see — resolve in
 * the child at http_request time. Unknown names and unbound/unmatched host
 * bindings throw before anything is sent. Host mode (no fork, no sandbox);
 * the success paths talk to a loopback server, never a real host. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "qjs_helpers.h"
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* ── one-shot loopback HTTP server ── */

typedef struct {
    int listen_fd;
    int port;
    char captured[4096];   /* raw request bytes */
} MockHttp;

static void *mock_http_thread(void *arg) {
    MockHttp *m = arg;
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(m->listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int c = accept(m->listen_fd, NULL, NULL);
    if (c < 0) return NULL;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recv(c, m->captured, sizeof(m->captured) - 1, 0);
    if (n > 0) m->captured[n] = '\0';
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                       "Connection: close\r\n\r\nok";
    send(c, resp, strlen(resp), 0);
    close(c);
    return NULL;
}

static int mock_http_start(MockHttp *m, pthread_t *tid) {
    memset(m, 0, sizeof(*m));
    m->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_fd < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(m->listen_fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(m->listen_fd, 1) != 0) { close(m->listen_fd); return -1; }
    socklen_t alen = sizeof(a);
    getsockname(m->listen_fd, (struct sockaddr *)&a, &alen);
    m->port = ntohs(a.sin_port);
    return pthread_create(tid, NULL, mock_http_thread, m);
}

static void mock_http_stop(MockHttp *m, pthread_t tid) {
    pthread_join(tid, NULL);
    close(m->listen_fd);
}

/* ── error paths (no server needed — the throw happens before any send) ── */

static const JsHostSecret SEC_BOUND = {
    .name = "TOK", .value = "sk-test-value-123", .hosts = "127.0.0.1" };
static const JsHostSecret SEC_UNBOUND = {
    .name = "TOK", .value = "sk-test-value-123", .hosts = NULL };
static const JsHostSecret SEC_OTHER_HOST = {
    .name = "TOK", .value = "sk-test-value-123", .hosts = "api.example.com .example.org" };

static void expect_js_error(const char *name, const JsHostSecret *sec,
                            const char *code, const char *needle) {
    tests_run++; printf("  %s... ", name);
    qjs_host_set_secrets(sec, sec ? 1 : 0);
    char args[1024];
    snprintf(args, sizeof(args), "{\"code\":%s}", code);
    char *r = test_js_eval_run_json(args);
    qjs_host_set_secrets(NULL, 0);
    if (!r || !strstr(r, needle)) { FAIL(r ? r : "NULL"); free(r); return; }
    free(r);
    PASS();
}

static void test_unknown_name(void) {
    expect_js_error("unknown_name", &SEC_BOUND,
        "\"http_request('http://127.0.0.1:1/x', {headers:{Authorization:'Bearer {{SECRET:TYPO}}'}})\"",
        "{{SECRET:TYPO}} names no known secret");
}

static void test_unbound_secret(void) {
    expect_js_error("unbound_secret", &SEC_UNBOUND,
        "\"http_request('http://127.0.0.1:1/x', {headers:{Authorization:'Bearer {{SECRET:TOK}}'}})\"",
        "secret TOK has no bound hosts");
}

static void test_wrong_host(void) {
    expect_js_error("wrong_host", &SEC_OTHER_HOST,
        "\"http_request('http://127.0.0.1:1/x', {headers:{Authorization:'Bearer {{SECRET:TOK}}'}})\"",
        "secret TOK is not bound to host 127.0.0.1");
}

static void test_no_secrets_loaded(void) {
    expect_js_error("no_secrets_loaded", NULL,
        "\"http_request('http://127.0.0.1:1/x', {headers:{Authorization:'Bearer {{SECRET:TOK}}'}})\"",
        "{{SECRET:TOK}} names no known secret");
}

/* ── success paths: placeholder resolves; server sees plaintext ── */

/* `where` builds the http_request call given host:port. */
static void expect_resolved(const char *name, const char *code_fmt,
                            const char *want_in_request) {
    tests_run++; printf("  %s... ", name);
    MockHttp m; pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock server"); return; }
    char code[2048];
    snprintf(code, sizeof(code), code_fmt, m.port);
    char args[4096];
    snprintf(args, sizeof(args), "{\"code\":\"%s\"}", code);
    qjs_host_set_secrets(&SEC_BOUND, 1);
    char *r = test_js_eval_run_json(args);
    qjs_host_set_secrets(NULL, 0);
    mock_http_stop(&m, tid);
    if (!r || strstr(r, "error")) { FAIL(r ? r : "NULL"); free(r); return; }
    if (!strstr(m.captured, want_in_request)) {
        FAIL("server did not receive the resolved value");
        printf("    got: %.300s\n", m.captured);
        free(r); return;
    }
    if (strstr(m.captured, "{{SECRET:")) {
        FAIL("literal placeholder reached the wire"); free(r); return;
    }
    free(r);
    PASS();
}

static void test_resolve_header(void) {
    expect_resolved("resolve_header",
        "var r = http_request('http://127.0.0.1:%d/h', "
        "{headers:{Authorization:'Bearer {{SECRET:TOK}}'}}); r.status",
        "Bearer sk-test-value-123");
}

static void test_resolve_body(void) {
    expect_resolved("resolve_body",
        "var r = http_request('http://127.0.0.1:%d/b', "
        "{method:'POST', body:'key={{SECRET:TOK}}'}); r.status",
        "key=sk-test-value-123");
}

static void test_resolve_url(void) {
    expect_resolved("resolve_url",
        "var r = http_request('http://127.0.0.1:%d/q?token={{SECRET:TOK}}'); r.status",
        "token=sk-test-value-123");
}

/* The originating case: the placeholder lives in a .qjs FILE body, invisible
 * to the parent's arg interpolation, and must still resolve at the fetch. */
static void test_resolve_from_file(void) {
    tests_run++; printf("  resolve_from_file... ");
    MockHttp m; pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock server"); return; }
    const char *path = "/tmp/test_js_secret_fetch_script.qjs";
    FILE *f = fopen(path, "w");
    if (!f) { FAIL("script write"); mock_http_stop(&m, tid); return; }
    fprintf(f,
        "var r = http_request('http://127.0.0.1:' + args.port + '/f', "
        "{headers:{'X-Api-Key':'{{SECRET:TOK}}'}}); r.status;\n");
    fclose(f);
    char args[512];
    snprintf(args, sizeof(args),
             "{\"filename\":\"%s\",\"args\":{\"port\":%d}}", path, m.port);
    qjs_host_set_secrets(&SEC_BOUND, 1);
    char *r = test_js_eval_run_json(args);
    qjs_host_set_secrets(NULL, 0);
    mock_http_stop(&m, tid);
    unlink(path);
    if (!r || strstr(r, "error")) { FAIL(r ? r : "NULL"); free(r); return; }
    if (!strstr(m.captured, "X-Api-Key: sk-test-value-123")) {
        FAIL("file-body placeholder did not resolve");
        printf("    got: %.300s\n", m.captured);
        free(r); return;
    }
    free(r);
    PASS();
}

/* No placeholder anywhere → nothing resolves, nothing throws, no binding
 * check (a loaded-but-unused secret must not constrain the request host). */
static void test_no_placeholder_no_gate(void) {
    tests_run++; printf("  no_placeholder_no_gate... ");
    MockHttp m; pthread_t tid;
    if (mock_http_start(&m, &tid) != 0) { FAIL("mock server"); return; }
    char args[512];
    /* SEC_OTHER_HOST is loaded but unused — 127.0.0.1 must still be legal. */
    snprintf(args, sizeof(args),
             "{\"code\":\"var r = http_request('http://127.0.0.1:%d/p'); r.status\"}",
             m.port);
    qjs_host_set_secrets(&SEC_OTHER_HOST, 1);
    char *r = test_js_eval_run_json(args);
    qjs_host_set_secrets(NULL, 0);
    mock_http_stop(&m, tid);
    if (!r || strstr(r, "error") || strcmp(r, "200") != 0) {
        FAIL(r ? r : "NULL"); free(r); return;
    }
    free(r);
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("test_js_secret_fetch:\n");

    test_unknown_name();
    test_unbound_secret();
    test_wrong_host();
    test_no_secrets_loaded();
    test_resolve_header();
    test_resolve_body();
    test_resolve_url();
    test_resolve_from_file();
    test_no_placeholder_no_gate();

    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
