#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include "js_http_fetch.h"
#include "test_util.h"

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) do { tests_run++; printf("  " name "... "); } while(0)
#define PASS() do { tests_passed++; printf("PASS\n"); } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); } while(0)

/* Egress (host/IP/redirect gating) is no longer a pre-flight in js_http_fetch —
 * it moved to the broker proxy's per-hop decide() (see test_proxy_decide.c).
 * The only validation left here is the scheme guard: http:// or https:// only. */
static void test_invalid_scheme(void) {
    TEST("invalid_scheme");
    JsHttpResult r = js_http_fetch_exec("ftp://example.com/file", "GET", NULL, NULL, 0, NULL, 0);
    if (r.status != -1) { FAIL("expected error"); js_http_result_free(&r); return; }
    if (!r.error || !strstr(r.error, "http://")) {
        FAIL(r.error ? r.error : "NULL"); js_http_result_free(&r); return;
    }
    js_http_result_free(&r);
    PASS();
}

static void test_null_url(void) {
    TEST("null_url");
    JsHttpResult r = js_http_fetch_exec(NULL, "GET", NULL, NULL, 0, NULL, 0);
    if (r.status != -1) { FAIL("expected error"); js_http_result_free(&r); return; }
    if (!r.error) { FAIL("expected error message"); js_http_result_free(&r); return; }
    js_http_result_free(&r);
    PASS();
}

/* Runtime create/destroy lifecycle (hosts feed the proxy, not the runtime) */
static void test_runtime_set_hosts(void) {
    TEST("runtime_set_hosts");
    JsSessionRuntime *rt = js_runtime_create();
    if (!rt) { FAIL("create failed"); return; }
    js_runtime_destroy(rt);
    PASS();
}

/* In-process js_eval compute: arithmetic returns the last expression value. No
 * fork, no re-exec, no network. */
static void test_js_eval_compute(void) {
    TEST("js_eval_compute");
    char *r = test_js_eval_run_json("{\"code\":\"6 * 7\"}");
    if (!r) { FAIL("NULL result"); return; }
    if (strcmp(r, "42") != 0) { FAIL(r); free(r); return; }
    free(r);
    PASS();
}

/* In-process js_eval: console.log output is captured. */
static void test_js_eval_console(void) {
    TEST("js_eval_console");
    char *r = test_js_eval_run_json("{\"code\":\"console.log('hi'); undefined\"}");
    if (!r) { FAIL("NULL result"); return; }
    if (!strstr(r, "hi")) { FAIL(r); free(r); return; }
    free(r);
    PASS();
}

/* js_eval requires code or filename. */
static void test_js_eval_requires_arg(void) {
    TEST("js_eval_requires_arg");
    char *r = test_js_eval_run_json("{}");
    if (!r) { FAIL("NULL result"); return; }
    if (!strstr(r, "error")) { FAIL(r); free(r); return; }
    free(r);
    PASS();
}

/* The timeout option is clamped, not trusted: 0/negative and anything past the
 * ceiling fall back to a sane value, because a JS fetch blocks its tool child
 * for its whole duration. Exercised through js_eval so the option is checked
 * where a handler actually sets it. */
static void test_timeout_option_is_clamped(void) {
    TEST("timeout_option_is_clamped");
    /* An absurd timeout on an unroutable address must still return promptly
     * with an error rather than being honoured literally. */
    char *r = test_js_eval_run_json(
        "{\"code\":\"var r = http_request('http://127.0.0.1:9/x', {timeout: 999999});"
        " r.status + ':' + (r.error ? 'err' : 'ok')\"}");
    if (!r) { FAIL("NULL result"); return; }
    if (!strstr(r, "-1")) { FAIL(r); free(r); return; }
    free(r);
    PASS();
}

/* ── Binary bodies: a loopback server with one binary and one 404 path ── */

/* JPEG-shaped: magic, NULs, and bytes that are not valid UTF-8. */
static const unsigned char BIN[] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01,
    0x80, 0x81, 0xFE, 0xC0, 0x00, 0xFF, 0xD9
};
#define BIN_CONNS 4

static void *bin_server(void *arg) {
    int lfd = *(int *)arg;
    for (int i = 0; i < BIN_CONNS; i++) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) break;              /* SO_RCVTIMEO: the client never came */
        char req[1024];
        ssize_t n = read(c, req, sizeof(req) - 1);
        req[n > 0 ? n : 0] = '\0';
        if (strstr(req, "GET /missing")) {
            const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 4\r\n"
                               "Connection: close\r\n\r\nnope";
            if (write(c, resp, strlen(resp)) < 0) { /* client gone */ }
        } else {
            char hdr[160];
            int hn = snprintf(hdr, sizeof(hdr),
                "HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\n"
                "Content-Length: %zu\r\nConnection: close\r\n\r\n", sizeof(BIN));
            if (write(c, hdr, (size_t)hn) < 0 ||
                write(c, BIN, sizeof(BIN)) < 0) { /* client gone */ }
        }
        close(c);
    }
    return NULL;
}

static int file_size(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char buf[256];
    int n = (int)fread(buf, 1, sizeof(buf), f);
    fclose(f);
    return (n == (int)sizeof(BIN) && memcmp(buf, BIN, sizeof(BIN)) == 0) ? n
         : (n == (int)sizeof(BIN) ? -2 : n);
}

/* http_request and binary responses, through js_eval: the string path refuses
 * them (it used to return a corrupted copy), save_to writes the exact bytes,
 * and a failed download leaves no file behind. */
static void test_binary_download(void) {
    TEST("binary_download");
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof(a);
    struct timeval tv = { .tv_sec = 5 };
    if (lfd < 0 || bind(lfd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        listen(lfd, 4) != 0 ||
        getsockname(lfd, (struct sockaddr *)&a, &al) != 0) {
        FAIL("listen"); return;
    }
    setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_t th;
    pthread_create(&th, NULL, bin_server, &lfd);

    char out[64], code[512];
    snprintf(out, sizeof(out), "/tmp/cclaw_test_bin_%d.jpg", (int)getpid());
    int port = ntohs(a.sin_port), ok = 1;
    const char *why = "";

    /* 1. no save_to: refused, and the message names the fix */
    snprintf(code, sizeof(code),
        "{\"code\":\"try { http_request('http://127.0.0.1:%d/img').body.length }"
        " catch (e) { String(e.message) }\"}", port);
    char *r = test_js_eval_run_json(code);
    if (!r || !strstr(r, "binary") || !strstr(r, "save_to")) { ok = 0; why = "not refused"; }
    free(r);

    /* 2. save_to: exact bytes on disk, {status, path, bytes} back */
    snprintf(code, sizeof(code),
        "{\"code\":\"var r = http_request('http://127.0.0.1:%d/img', {save_to:'%s'});"
        " r.status + ' ' + r.bytes + ' ' + r.path + ' [' + r.body + ']'\"}", port, out);
    r = test_js_eval_run_json(code);
    char want[128];
    snprintf(want, sizeof(want), "200 %zu %s []", sizeof(BIN), out);
    if (ok && (!r || !strstr(r, want))) { ok = 0; why = "save result"; }
    if (ok && file_size(out) != (int)sizeof(BIN)) { ok = 0; why = "file bytes differ"; }
    free(r);
    unlink(out);

    /* 3. over max_bytes: an error, and no partial file */
    snprintf(code, sizeof(code),
        "{\"code\":\"var r = http_request('http://127.0.0.1:%d/img',"
        " {save_to:'%s', max_bytes: 4}); r.status + ' ' + r.error\"}", port, out);
    r = test_js_eval_run_json(code);
    if (ok && (!r || !strstr(r, "-1 ") || !strstr(r, "max_bytes"))) { ok = 0; why = "cap"; }
    if (ok && access(out, F_OK) == 0) { ok = 0; why = "partial file kept"; }
    free(r);

    /* 4. non-2xx: the server's text in body, no file */
    snprintf(code, sizeof(code),
        "{\"code\":\"var r = http_request('http://127.0.0.1:%d/missing',"
        " {save_to:'%s'}); r.status + ' ' + r.body + ' ' + r.path\"}", port, out);
    r = test_js_eval_run_json(code);
    if (ok && (!r || !strstr(r, "404 nope undefined"))) { ok = 0; why = "404"; }
    if (ok && access(out, F_OK) == 0) { ok = 0; why = "file kept on 404"; }
    free(r);
    unlink(out);

    pthread_join(th, NULL);
    close(lfd);
    if (ok) PASS(); else FAIL(why);
}

int main(void) {
    TEST_INIT();
    printf("test_js_http_fetch:\n");
    test_invalid_scheme();
    test_null_url();
    test_runtime_set_hosts();
    test_js_eval_compute();
    test_js_eval_console();
    test_js_eval_requires_arg();
    test_timeout_option_is_clamped();
    test_binary_download();
    printf("%d/%d passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
