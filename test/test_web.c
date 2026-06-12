#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
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

    assert(web_start(&cfg, db, cfg.db_path) == 0);
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

/* Fake channel runner: accept one UDS connection, capture the envelope,
 * reply "207\nhook-reply". Forked child so the blocking proxy thread in
 * web.c has a peer. */
static pid_t start_fake_runner(const char *sock_path) {
    unlink(sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
    assert(bind(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    assert(listen(fd, 1) == 0);
    struct timeval tv = {10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    pid_t pid = fork();
    if (pid == 0) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd < 0) _exit(1);
        char buf[16384];
        size_t len = 0;
        for (;;) {
            ssize_t n = read(cfd, buf + len, sizeof(buf) - len - 1);
            if (n > 0) { len += (size_t)n; continue; }
            break;
        }
        buf[len] = '\0';
        /* Envelope must carry method, path and the body we POSTed */
        int ok = strstr(buf, "\"method\":\"POST\"") &&
                 strstr(buf, "\"path\":\"/hook/tchan\"") &&
                 strstr(buf, "hello-hook") &&
                 strstr(buf, "X-Hook-Test");
        const char *reply = ok ? "207\nhook-reply" : "500\nbad-envelope";
        (void)write(cfd, reply, strlen(reply));
        close(cfd);
        close(fd);
        _exit(ok ? 0 : 2);
    }
    close(fd);
    return pid;
}

static void test_hook_proxy(void) {
    Config cfg = {0};
    cfg.web_port = 19877;
    cfg.db_path = "test_web_hook.db";

    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);
    assert(web_start(&cfg, db, cfg.db_path) == 0);
    usleep(50000);

    /* channel_uds_path("test_web_hook.db","tchan") = test_web_hook.tchan.sock */
    pid_t runner = start_fake_runner("test_web_hook.tchan.sock");
    assert(runner > 0);
    usleep(50000);

    CURL *curl = curl_easy_init();
    assert(curl);
    char response[4096] = {0};
    struct curl_slist *hdrs = curl_slist_append(NULL, "X-Hook-Test: yes");
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19877/hook/tchan");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{\"msg\":\"hello-hook\"}");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
    CURLcode res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 207);
    assert(strcmp(response, "hook-reply") == 0);

    int status = -1;
    waitpid(runner, &status, 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    /* No runner socket → 502 */
    char response2[4096] = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19877/hook/nosuch");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response2);
    res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 502);

    /* Bad channel name → 404 */
    char response3[4096] = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19877/hook/bad$chan");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response3);
    res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 404);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    remove("test_web_hook.db");
    unlink("test_web_hook.tchan.sock");
    printf("PASS: test_hook_proxy\n");
}

int main(void) {
    test_status_page();
    test_hook_proxy();
    printf("All web tests passed.\n");
    return 0;
}
