#define _GNU_SOURCE
#include <assert.h>
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

/* Egress (host/IP/redirect gating) is no longer a pre-flight in js_http_fetch —
 * it moved to the broker proxy's per-hop decide() (see test_proxy_decide.c).
 * The only validation left here is the scheme guard: http:// or https:// only. */
static void test_invalid_scheme(void) {
    JsHttpResult r = js_http_fetch_exec("ftp://example.com/file", "GET", NULL, NULL, 0, NULL, 0, NULL);
    assert(r.status == -1);
    assert(r.error && strstr(r.error, "http://"));
    js_http_result_free(&r);
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
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET,
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof(a);
    struct timeval tv = { .tv_sec = 5 };
    assert(lfd >= 0 && bind(lfd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
           listen(lfd, 4) == 0 &&
           getsockname(lfd, (struct sockaddr *)&a, &al) == 0);
    setsockopt(lfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    pthread_t th;
    pthread_create(&th, NULL, bin_server, &lfd);

    char out[64], code[512];
    snprintf(out, sizeof(out), "/tmp/cclaw_test_bin_%d.jpg", (int)getpid());
    int port = ntohs(a.sin_port);

    /* 1. no save_to: refused, and the message names the fix */
    snprintf(code, sizeof(code),
        "{\"code\":\"try { http_request('http://127.0.0.1:%d/img').body.length }"
        " catch (e) { String(e.message) }\"}", port);
    char *r = test_js_eval_run_json(code);
    assert(r && strstr(r, "binary") && strstr(r, "save_to"));
    free(r);

    /* 2. save_to: exact bytes on disk, {status, path, bytes} back */
    snprintf(code, sizeof(code),
        "{\"code\":\"var r = http_request('http://127.0.0.1:%d/img', {save_to:'%s'});"
        " r.status + ' ' + r.bytes + ' ' + r.path + ' [' + r.body + ']'\"}", port, out);
    r = test_js_eval_run_json(code);
    char want[128];
    snprintf(want, sizeof(want), "200 %zu %s []", sizeof(BIN), out);
    assert(r && strstr(r, want));
    assert(file_size(out) == (int)sizeof(BIN));
    free(r);
    unlink(out);

    /* 3. over max_bytes: an error, and no partial file */
    snprintf(code, sizeof(code),
        "{\"code\":\"var r = http_request('http://127.0.0.1:%d/img',"
        " {save_to:'%s', max_bytes: 4}); r.status + ' ' + r.error\"}", port, out);
    r = test_js_eval_run_json(code);
    assert(r && strstr(r, "-1 ") && strstr(r, "max_bytes"));
    assert(access(out, F_OK) != 0);
    free(r);

    /* 4. non-2xx: the server's text in body, no file */
    snprintf(code, sizeof(code),
        "{\"code\":\"var r = http_request('http://127.0.0.1:%d/missing',"
        " {save_to:'%s'}); r.status + ' ' + r.body + ' ' + r.path\"}", port, out);
    r = test_js_eval_run_json(code);
    assert(r && strstr(r, "404 nope undefined"));
    assert(access(out, F_OK) != 0);
    free(r);
    unlink(out);

    pthread_join(th, NULL);
    close(lfd);
}

int main(void) {
    TEST_INIT();
    printf("test_js_http_fetch:\n");
    RUN(test_invalid_scheme);
    RUN(test_binary_download);
    return 0;
}
