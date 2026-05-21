#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <curl/curl.h>
#include "web.h"
#include "db.h"

static size_t write_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    char *buf = (char *)userdata;
    size_t cur = strlen(buf);
    if (cur + total < 4096) {
        memcpy(buf + cur, ptr, total);
        buf[cur + total] = '\0';
    }
    return total;
}

static void test_web_start_stop(void) {
    Config cfg = {0};
    cfg.web_port = 19876;
    cfg.db_path = "test_web.db";

    sqlite3 *db = db_open(cfg.db_path);
    assert(db);

    assert(web_start(&cfg, db) == 0);

    /* Give server a moment to bind */
    usleep(50000);

    /* HTTP GET / */
    CURL *curl = curl_easy_init();
    assert(curl);
    char response[4096] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19876/");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 200);
    assert(strstr(response, "\"version\"") != NULL);
    assert(strstr(response, "\"uptime_seconds\"") != NULL);
    assert(strstr(response, "\"sessions\"") != NULL);
    curl_easy_cleanup(curl);

    web_stop();
    db_close(db);
    remove("test_web.db");
    printf("PASS: test_web_start_stop\n");
}

int main(void) {
    test_web_start_stop();
    printf("All web tests passed.\n");
    return 0;
}
