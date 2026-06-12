#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <curl/curl.h>
#include "web.h"
#include "db.h"
#include "test_util.h"

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

static void test_status_page(void) {
    Config cfg = {0};
    cfg.web_port = 19876;
    cfg.db_path = "test_web.db";

    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);

    int64_t sid = session_create(db, "test-session", NULL, -1, 0);
    assert(sid > 0);
    assert(session_set_state(db, sid, "llm_running") == 0);
    assert(inbox_insert(db, sid, "test", "hello") > 0);

    assert(web_start(&cfg, db) == 0);
    usleep(50000);

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

    /* Plain text assertions */
    assert(strstr(response, "version:"));
    assert(strstr(response, "uptime:"));
    assert(strstr(response, "sessions:\n"));
    assert(strstr(response, "id|name|state|updated_at|inbox_depth\n"));
    assert(strstr(response, "test-session"));
    assert(strstr(response, "running"));
    assert(strstr(response, "state_metrics:\n"));
    assert(strstr(response, "running: 1"));

    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    remove("test_web.db");
    printf("PASS: test_status_page\n");
}

int main(void) {
    test_status_page();
    printf("All web tests passed.\n");
    return 0;
}
