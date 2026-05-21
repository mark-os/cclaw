#include "http.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <curl/curl.h>

static void test_response_free_empty(void) {
    HttpResponse resp = {0};
    http_response_free(&resp);
    assert(resp.data == NULL);
    assert(resp.len == 0);
    assert(resp.cap == 0);
}

static void test_post_bad_url(void) {
    HttpResponse resp = {0};
    const char *headers[] = {"Content-Type: application/json", NULL};
    int status = http_post("http://127.0.0.1:1", headers, "{}", &resp);
    /* Connection refused → curl error → -1 */
    assert(status == -1);
    http_response_free(&resp);
}

static void test_post_real(void) {
    /* POST to httpbin — only run if network available, skip gracefully */
    HttpResponse resp = {0};
    const char *headers[] = {"Content-Type: application/json", NULL};
    int status = http_post("https://httpbin.org/post", headers,
                           "{\"test\":true}", &resp);
    if (status == 200) {
        assert(resp.data != NULL);
        assert(resp.len > 0);
        assert(strstr(resp.data, "\"test\"") != NULL);
    }
    /* If network unavailable, status == -1, that's OK for CI */
    http_response_free(&resp);
}

int main(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    test_response_free_empty();
    printf("PASS: response_free_empty\n");

    test_post_bad_url();
    printf("PASS: post_bad_url\n");

    test_post_real();
    printf("PASS: post_real (or skipped)\n");

    curl_global_cleanup();
    printf("All http tests passed.\n");
    return 0;
}
