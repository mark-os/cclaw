#include "http.h"
#include "mock_server.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

static void test_response_free_empty(void) {
    HttpResponse resp = {0};
    http_response_free(&resp);
    assert(resp.data == NULL);
    assert(resp.len == 0);
}

static void test_post_bad_url(void) {
    HttpResponse resp = {0};
    const char *headers[] = {"Content-Type: application/json", NULL};
    int status = http_post("http://127.0.0.1:1", headers, "{}", &resp);
    /* Connection refused → curl error → -1 */
    assert(status == -1);
    http_response_free(&resp);
}

/* A body larger than max_response_bytes is truncated to the cap and reported
 * as a successful, flagged transfer — not a CURLE_WRITE_ERROR that drops it. */
static void test_cap_truncates_not_errors(int port) {
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", port);

    size_t big = 4000;
    char *body = malloc(big + 1);
    assert(body);
    memset(body, 'a', big);
    body[big] = '\0';
    mock_server_enqueue(200, body);
    free(body);

    HttpResponse resp = {0};
    HttpRequestOpts opts = { .url = url, .method = "GET", .max_response_bytes = 1000 };
    int status = http_do(&opts, &resp);

    assert(status == 200);       /* recovered, real HTTP status preserved */
    assert(resp.truncated == 1); /* flagged */
    assert(resp.len == 1000);    /* clipped exactly at the cap */
    assert(resp.data != NULL);   /* partial body survives */
    http_response_free(&resp);
}

/* A body within the cap comes back whole with truncated == 0. */
static void test_within_cap_not_truncated(int port) {
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", port);

    mock_server_enqueue(200, "hello world");
    HttpResponse resp = {0};
    HttpRequestOpts opts = { .url = url, .method = "GET", .max_response_bytes = 1000 };
    int status = http_do(&opts, &resp);

    assert(status == 200);
    assert(resp.truncated == 0);
    assert(resp.len == 11);
    http_response_free(&resp);
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    test_response_free_empty();
    printf("PASS: response_free_empty\n");

    test_post_bad_url();
    printf("PASS: post_bad_url\n");

    int port = mock_server_start();
    assert(port > 0);
    test_cap_truncates_not_errors(port);
    printf("PASS: cap_truncates_not_errors\n");
    test_within_cap_not_truncated(port);
    printf("PASS: within_cap_not_truncated\n");
    mock_server_stop();

    curl_global_cleanup();
    printf("All http tests passed.\n");
    return 0;
}
