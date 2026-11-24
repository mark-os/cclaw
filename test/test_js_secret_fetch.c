/* Fetch-boundary {{SECRET:name}} resolution in the js child (qjs_host_eval.c):
 * placeholders in url/body/headers — including ones read from a .qjs file
 * body, the case the parent's arg interpolation can never see — resolve in
 * the child at http_request time. Unknown names and unbound/unmatched host
 * bindings throw before anything is sent. Host mode (no fork, no sandbox);
 * the success paths talk to a loopback server, never a real host. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "qjs_helpers.h"
#include "test_util.h"

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

/* Each refusal names the secret and why; the keyword is enough. */
static void test_error_paths(void) {
    static const struct {
        const char *name;
        const JsHostSecret *sec;
        const char *placeholder;
        const char *needle;
    } cases[] = {
        { "unknown_name",      &SEC_BOUND,      "TYPO", "{{SECRET:TYPO}} names no secret" },
        { "unbound_secret",    &SEC_UNBOUND,    "TOK",  "no bound hosts" },
        { "wrong_host",        &SEC_OTHER_HOST, "TOK",  "not bound to host 127.0.0.1" },
        { "no_secrets_loaded", NULL,            "TOK",  "{{SECRET:TOK}} names no secret" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("    %s\n", cases[i].name);
        qjs_host_set_secrets(cases[i].sec, cases[i].sec ? 1 : 0);
        char args[1024];
        snprintf(args, sizeof(args),
                 "{\"code\":\"http_request('http://127.0.0.1:1/x', {headers:"
                 "{Authorization:'Bearer {{SECRET:%s}}'}})\"}", cases[i].placeholder);
        char *r = test_js_eval_run_json(args);
        qjs_host_set_secrets(NULL, 0);
        if (!r || !strstr(r, cases[i].needle))
            printf("    row %s got: %s\n", cases[i].name, r ? r : "NULL");
        assert(r && strstr(r, cases[i].needle));
        free(r);
    }
}

/* ── success paths: placeholder resolves; server sees plaintext ── */

/* Each code_fmt builds the http_request call given the mock port. */
static void test_resolve_paths(void) {
    static const struct {
        const char *name;
        const char *code_fmt;
        const char *want_in_request;
    } cases[] = {
        { "resolve_header",
          "var r = http_request('http://127.0.0.1:%d/h', "
          "{headers:{Authorization:'Bearer {{SECRET:TOK}}'}}); r.status",
          "Bearer sk-test-value-123" },
        { "resolve_body",
          "var r = http_request('http://127.0.0.1:%d/b', "
          "{method:'POST', body:'key={{SECRET:TOK}}'}); r.status",
          "key=sk-test-value-123" },
        { "resolve_url",
          "var r = http_request('http://127.0.0.1:%d/q?token={{SECRET:TOK}}'); r.status",
          "token=sk-test-value-123" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        printf("    %s\n", cases[i].name);
        MockHttp m; pthread_t tid;
        assert(mock_http_start(&m, &tid) == 0);
        char code[2048];
        snprintf(code, sizeof(code), cases[i].code_fmt, m.port);
        char args[4096];
        snprintf(args, sizeof(args), "{\"code\":\"%s\"}", code);
        qjs_host_set_secrets(&SEC_BOUND, 1);
        char *r = test_js_eval_run_json(args);
        qjs_host_set_secrets(NULL, 0);
        mock_http_stop(&m, tid);
        assert(r && !strstr(r, "error"));
        assert(strstr(m.captured, cases[i].want_in_request));
        assert(!strstr(m.captured, "{{SECRET:"));
        free(r);
    }
}

/* The originating case: the placeholder lives in a .qjs FILE body, invisible
 * to the parent's arg interpolation, and must still resolve at the fetch. */
static void test_resolve_from_file(void) {
    MockHttp m; pthread_t tid;
    assert(mock_http_start(&m, &tid) == 0);
    const char *path = "/tmp/test_js_secret_fetch_script.qjs";
    FILE *f = fopen(path, "w");
    assert(f);
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
    assert(r && !strstr(r, "error"));
    assert(strstr(m.captured, "X-Api-Key: sk-test-value-123"));
    free(r);
}

/* No placeholder anywhere → nothing resolves, nothing throws, no binding
 * check (a loaded-but-unused secret must not constrain the request host). */
static void test_no_placeholder_no_gate(void) {
    MockHttp m; pthread_t tid;
    assert(mock_http_start(&m, &tid) == 0);
    char args[512];
    /* SEC_OTHER_HOST is loaded but unused — 127.0.0.1 must still be legal. */
    snprintf(args, sizeof(args),
             "{\"code\":\"var r = http_request('http://127.0.0.1:%d/p'); r.status\"}",
             m.port);
    qjs_host_set_secrets(&SEC_OTHER_HOST, 1);
    char *r = test_js_eval_run_json(args);
    qjs_host_set_secrets(NULL, 0);
    mock_http_stop(&m, tid);
    assert(r && strcmp(r, "200") == 0);
    free(r);
}

int main(void) {
    TEST_INIT();
    printf("test_js_secret_fetch:\n");
    RUN(test_error_paths);
    RUN(test_resolve_paths);
    RUN(test_resolve_from_file);
    RUN(test_no_placeholder_no_gate);
    return 0;
}
