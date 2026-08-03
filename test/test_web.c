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
#include "config_registry.h"
#include "db.h"
#include "resolve.h"
#include "test_util.h"

/* dashboard.o (pulled in via web.o) references resolve_approval, normally
 * defined in main.c — stub it and capture the call, same pattern as
 * test_channel_events.c. */
static int64_t g_ra_id = -1;
static ApprovalDecision g_ra_decision;
static char g_ra_via[32];
void resolve_approval(int64_t approval_id, ApprovalDecision decision,
                      const char *decided_via, int64_t grant_expires_at) {
    (void)grant_expires_at;
    g_ra_id = approval_id;
    g_ra_decision = decision;
    snprintf(g_ra_via, sizeof(g_ra_via), "%s", decided_via ? decided_via : "");
}

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
    assert(inbox_insert(db, sid, "test", NULL, "hello") > 0);

    assert(web_start(&cfg, db, cfg.db_path) == 0);
    usleep(50000);

    /* Unauthenticated `/` is liveness only — no session table (r2 F3). */
    CURL *curl = curl_easy_init();
    assert(curl);
    char response[4096] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19876/");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);
    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 200);
    assert(strstr(response, "version:"));
    assert(strstr(response, "uptime:"));
    assert(strstr(response, "sessions:") == NULL);
    assert(strstr(response, "test-session") == NULL);
    curl_easy_cleanup(curl);

    /* With the dashboard admin token (?token=), the full dump is served. */
    char *tok = config_get(db, "web_admin_token");
    assert(tok && tok[0]);
    char url[256];
    snprintf(url, sizeof(url), "http://127.0.0.1:19876/?token=%s", tok);
    free(tok);
    curl = curl_easy_init();
    assert(curl);
    response[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);
    res = curl_easy_perform(curl);
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
    test_db_clean("test_web.db");
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
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response);
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
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response2);
    res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 502);

    /* Bad channel name → 404 */
    char response3[4096] = {0};
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:19877/hook/bad$chan");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)response3);
    res = curl_easy_perform(curl);
    assert(res == CURLE_OK);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    assert(http_code == 404);

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    test_db_clean("test_web_hook.db");
    unlink("test_web_hook.tchan.sock");
    printf("PASS: test_hook_proxy\n");
}

/* ── admin dashboard ────────────────────────────────────────────── */

struct page_buf { char *buf; size_t cap; };

static size_t page_cb(void *ptr, size_t size, size_t nmemb, void *userdata) {
    size_t total = size * nmemb;
    struct page_buf *b = (struct page_buf *)userdata;
    size_t cur = strlen(b->buf);
    if (cur + total < b->cap) {
        memcpy(b->buf + cur, ptr, total);
        b->buf[cur + total] = '\0';
    }
    return total;
}

static char *query_text(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    char *out = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return NULL;
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *)sqlite3_column_text(st, 0);
        if (v) out = strdup(v);
    }
    sqlite3_finalize(st);
    return out;
}

/* GET url into page (cleared first); returns final HTTP code. */
static long dash_get(CURL *curl, const char *url, char *page, size_t cap) {
    struct page_buf pb = {page, cap};
    page[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, page_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &pb);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

/* POST form to /admin/act without following the 303; returns HTTP code. */
static long dash_post(CURL *curl, const char *url, const char *form,
                      char *page, size_t cap) {
    struct page_buf pb = {page, cap};
    page[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, page_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &pb);
    assert(curl_easy_perform(curl) == CURLE_OK);
    long code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
    return code;
}

static void test_admin_dashboard(void) {
    Config cfg = {0};
    cfg.web_port = 19878;
    cfg.db_path = "test_web_admin.db";

    sqlite3 *db = test_db_open(cfg.db_path);
    assert(db);
    assert(sqlite3_exec(db,
        "INSERT OR REPLACE INTO providers(name, base_url, api_key_env) VALUES"
        "  ('openrouter','https://openrouter.example/v1','OPENROUTER_API_KEY');"
        "INSERT OR REPLACE INTO models(id, provider_name, model, priority) VALUES"
        "  ('m1','openrouter','vendor/one',0), ('m2','openrouter','vendor/two',1);"
        "INSERT OR IGNORE INTO agents(name) VALUES('Assistant'), ('ev<il&name');"
        "INSERT INTO sessions(name, agent_name, channel_name)"
        "  VALUES('s','Assistant','telegram');"
        "INSERT INTO approvals(session_id, tool_name, action, args_json, state)"
        "  VALUES(1, 'shell_exec', 'exec', '{\"cmd\":\"ls\"}', 'pending');",
        NULL, NULL, NULL) == SQLITE_OK);

    assert(web_start(&cfg, db, cfg.db_path) == 0);
    usleep(50000);

    /* web_start minted a token */
    char *token = query_text(db, "SELECT value FROM config WHERE key='web_admin_token'");
    assert(token && strlen(token) == 32);

    static char page[65536];
    CURL *curl = curl_easy_init();
    assert(curl);
    curl_easy_setopt(curl, CURLOPT_COOKIEFILE, "");  /* enable cookie engine */

    /* No token → 403 */
    assert(dash_get(curl, "http://127.0.0.1:19878/admin", page, sizeof(page)) == 403);

    /* Bad token → 403 */
    assert(dash_get(curl, "http://127.0.0.1:19878/admin?token=00000000000000000000000000000000",
                    page, sizeof(page)) == 403);

    /* Good token → 303 sets cookie, follow lands on the page */
    char url[160];
    snprintf(url, sizeof(url), "http://127.0.0.1:19878/admin?token=%s", token);
    assert(dash_get(curl, url, page, sizeof(page)) == 200);
    assert(strstr(page, "cclaw admin"));
    assert(strstr(page, "m1"));
    assert(strstr(page, "primary model"));
    assert(strstr(page, "shell_exec"));         /* pending approval row */

    /* Cookie now carries auth on its own */
    assert(dash_get(curl, "http://127.0.0.1:19878/admin", page, sizeof(page)) == 200);

    /* Hostile agent name is HTML-escaped everywhere */
    assert(strstr(page, "ev&lt;il&amp;name"));
    assert(strstr(page, "ev<il&name") == NULL);

    /* switch_model makes m2 the routing head */
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act",
                     "action=switch_model&model_id=m2", page, sizeof(page)) == 303);
    char *head = query_text(db, "SELECT id FROM models ORDER BY priority LIMIT 1");
    assert(head && strcmp(head, "m2") == 0);
    free(head);

    /* grant → row exists; revoke by rowid → gone */
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act",
                     "action=grant&agent=Assistant&kind=tool&value=web_fetch",
                     page, sizeof(page)) == 303);
    char *gid = query_text(db,
        "SELECT rowid FROM grants WHERE agent_name='Assistant' AND value='web_fetch'");
    assert(gid);
    char form[128];
    snprintf(form, sizeof(form), "action=revoke&agent=Assistant&grant_id=%s", gid);
    free(gid);
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act", form,
                     page, sizeof(page)) == 303);
    char *gone = query_text(db,
        "SELECT rowid FROM grants WHERE agent_name='Assistant' AND value='web_fetch'");
    assert(gone == NULL);

    /* approve routes through resolve_approval with decided_via="dashboard" */
    assert(dash_post(curl, "http://127.0.0.1:19878/admin/act",
                     "action=approve&approval_id=1&decision=always",
                     page, sizeof(page)) == 303);
    assert(g_ra_id == 1);
    assert(g_ra_decision == APPROVAL_ALWAYS);
    assert(strcmp(g_ra_via, "dashboard") == 0);

    /* POST without the cookie → 403 (query token is GET-only) */
    CURL *bare = curl_easy_init();
    assert(bare);
    assert(dash_post(bare, "http://127.0.0.1:19878/admin/act",
                     "action=switch_model&model_id=m1", page, sizeof(page)) == 403);
    curl_easy_cleanup(bare);

    free(token);
    curl_easy_cleanup(curl);
    web_stop();
    db_close(db);
    test_db_clean("test_web_admin.db");
    printf("PASS: test_admin_dashboard\n");
}

int main(void) {
    TEST_INIT();
    test_status_page();
    test_hook_proxy();
    test_admin_dashboard();
    printf("All web tests passed.\n");
    return 0;
}
