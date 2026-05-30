#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <curl/curl.h>
#include "mock_server.h"

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    char *buf = (char *)userdata;
    size_t cur = strlen(buf);
    if (cur + total < 8192) {
        memcpy(buf + cur, ptr, total);
        buf[cur + total] = '\0';
    }
    return total;
}

static void test_start_stop(void) {
    int port = mock_server_start();
    assert(port > 0);
    mock_server_stop();
    printf("PASS: test_start_stop\n");
}

static void test_enqueue_and_fetch(void) {
    int port = mock_server_start();
    assert(port > 0);

    const char *resp_body = "{\"id\":\"chatcmpl-1\",\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"hello\"},\"finish_reason\":\"stop\"}]}";
    mock_server_enqueue(200, resp_body);

    /* POST to the mock endpoint */
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", port);

    CURL *curl = curl_easy_init();
    assert(curl);
    char response[8192] = {0};
    const char *req = "{\"model\":\"test\",\"messages\":[]}";
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);

    struct curl_slist *hdrs = curl_slist_append(NULL, "Content-Type: application/json");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode res = curl_easy_perform(curl);
    assert(res == CURLE_OK);

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 200);
    assert(strstr(response, "chatcmpl-1") != NULL);
    assert(mock_server_request_count() == 1);
    assert(mock_server_last_request_body() != NULL);
    assert(strstr(mock_server_last_request_body(), "\"model\":\"test\"") != NULL);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    mock_server_stop();
    printf("PASS: test_enqueue_and_fetch\n");
}

static void test_multiple_responses(void) {
    int port = mock_server_start();
    assert(port > 0);

    mock_server_enqueue(200, "{\"n\":1}");
    mock_server_enqueue(429, "{\"error\":\"rate limited\"}");
    mock_server_enqueue(200, "{\"n\":3}");

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", port);

    CURL *curl = curl_easy_init();
    char response[8192];

    /* First: 200 */
    memset(response, 0, sizeof(response));
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long code;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    assert(code == 200);
    assert(strstr(response, "\"n\":1"));

    /* Second: 429 */
    memset(response, 0, sizeof(response));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    assert(code == 429);

    /* Third: 200 */
    memset(response, 0, sizeof(response));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    assert(code == 200);
    assert(strstr(response, "\"n\":3"));

    /* Fourth: queue empty → 500 */
    memset(response, 0, sizeof(response));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    assert(code == 500);

    assert(mock_server_request_count() == 4);

    curl_easy_cleanup(curl);
    mock_server_stop();
    printf("PASS: test_multiple_responses\n");
}

static void test_load_template(void) {
    int port = mock_server_start();
    assert(port > 0);

    int loaded = mock_server_load("test/fixtures/mock_responses.json");
    assert(loaded == 3);

    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/v1/chat/completions", port);

    CURL *curl = curl_easy_init();
    char response[8192];
    long code;

    /* First: 200 with JSON body */
    memset(response, 0, sizeof(response));
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    assert(code == 200);
    assert(strstr(response, "hello from template") != NULL);

    /* Second: 429 */
    memset(response, 0, sizeof(response));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    assert(code == 429);

    /* Third: 200 with string body */
    memset(response, 0, sizeof(response));
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    assert(curl_easy_perform(curl) == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    assert(code == 200);
    assert(strstr(response, "raw string body") != NULL);

    curl_easy_cleanup(curl);
    mock_server_stop();
    printf("PASS: test_load_template\n");
}

int main(void) {
    test_start_stop();
    test_enqueue_and_fetch();
    test_multiple_responses();
    test_load_template();
    printf("All mock_server tests passed.\n");
    return 0;
}
